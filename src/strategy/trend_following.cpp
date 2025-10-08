// src/strategy/trend_following.cpp
#include "trade_ngin/strategy/trend_following.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <numeric>
#include <set>

namespace trade_ngin {

TrendFollowingStrategy::TrendFollowingStrategy(std::string id, StrategyConfig config,
                                               TrendFollowingConfig trend_config,
                                               std::shared_ptr<PostgresDatabase> db,
                                               std::shared_ptr<InstrumentRegistry> registry)
    : BaseStrategy(std::move(id), std::move(config), std::move(db)),
      trend_config_(std::move(trend_config)),
      registry_(registry) {
    Logger::register_component("TrendFollowing");

    // Verify lengths of lookback periods
    if (trend_config_.vol_lookback_short <= 0) {
        trend_config_.vol_lookback_short = 22;
    }
    if (trend_config_.vol_lookback_long <= trend_config_.vol_lookback_short) {
        trend_config_.vol_lookback_long = trend_config_.vol_lookback_short * 4;
    }

    // Initialize metadata
    metadata_.name = "Trend Following Strategy";
    metadata_.description = "Multi-timeframe trend following using EMA crossovers";
}

Result<void> TrendFollowingStrategy::validate_config() const {
    auto result = BaseStrategy::validate_config();
    if (result.is_error())
        return result;

    // Validate trend-specific config
    if (trend_config_.risk_target <= 0.0 || trend_config_.risk_target > 1.0) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT, "Risk target must be between 0 and 1",
                                "TrendFollowingStrategy");
    }

    if (trend_config_.idm <= 0.0) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT, "IDM must be positive",
                                "TrendFollowingStrategy");
    }

    if (trend_config_.ema_windows.empty()) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Must specify at least one EMA window pair",
                                "TrendFollowingStrategy");
    }

    return Result<void>();
}

Result<void> TrendFollowingStrategy::initialize() {
    // Call base class initialization first
    auto base_result = BaseStrategy::initialize();
    if (base_result.is_error()) {
        std::cerr << "Base strategy initialization failed: " << base_result.error()->what()
                  << std::endl;
        return base_result;
    }

    // Set PnL accounting method for futures (marked-to-market daily)
    set_pnl_accounting_method(PnLAccountingMethod::REALIZED_ONLY);
    INFO("Trend following strategy initialized with REALIZED_ONLY PnL accounting for futures");

    try {
        // Initialize price history containers for each symbol with proper capacity
        for (const auto& [symbol, _] : config_.trading_params) {
            try {
                price_history_[symbol].reserve(std::max(trend_config_.vol_lookback_long, 2520));
                volatility_history_[symbol].reserve(
                    std::max(trend_config_.vol_lookback_long, 2520));
            } catch (const std::exception& e) {
                std::cerr << "Failed to reserve price history for symbol " << symbol << ": "
                          << e.what() << std::endl;
                return make_error<void>(
                    ErrorCode::INVALID_ARGUMENT,
                    std::string("Failed to reserve price history for symbol ") + symbol,
                    "TrendFollowingStrategy");
            }

            // Initialize positions with zero quantity
            Position pos;
            pos.symbol = symbol;
            pos.quantity = 0.0;
            pos.average_price = 1.0;
            pos.last_update = std::chrono::system_clock::now();
            positions_[symbol] = pos;
        }

        return Result<void>();

    } catch (const std::exception& e) {
        std::cerr << "Error in TrendFollowingStrategy::initialize: " << e.what() << std::endl;
        return make_error<void>(
            ErrorCode::STRATEGY_ERROR,
            std::string("Failed to initialize trend following strategy: ") + e.what(),
            "TrendFollowingStrategy");
    }
}

Result<void> TrendFollowingStrategy::on_data(const std::vector<Bar>& data) {
    // Validate data
    if (data.empty()) {
        return Result<void>();
    }


    // Load previous positions if not already loaded (only once per run)
    static bool previous_positions_loaded = false;
    if (!previous_positions_loaded && previous_positions_.empty() && db_) {
        // Use the data's timestamp (not current time) to handle historical runs correctly
        // Get the timestamp from the first bar to determine the processing date
        auto data_time = data.empty() ? std::chrono::system_clock::now() : data[0].timestamp;
        auto previous_date = data_time - std::chrono::hours(24);
        auto previous_positions_result = db_->load_positions_by_date(id_, previous_date, "trading.positions");
        
        if (previous_positions_result.is_ok()) {
            const auto& previous_positions = previous_positions_result.value();
            INFO("Loaded " + std::to_string(previous_positions.size()) + " previous day positions for PnL calculation");
            previous_positions_ = previous_positions;
        } else {
            INFO("No previous day positions found (first run or no data): " + std::string(previous_positions_result.error()->what()));
        }
        previous_positions_loaded = true;
    }

    // Call base class data processing first
    auto base_result = BaseStrategy::on_data(data);
    if (base_result.is_error())
        return base_result;

    // Get longest window in ema pairs
    int max_window = 0;
    for (const auto& window_pair : trend_config_.ema_windows) {
        max_window = std::max(max_window, window_pair.second);
    }

    const size_t MAX_HISTORY_SIZE = 2500;

    try {
        // Group data by symbol and update price history
        std::unordered_map<std::string, std::vector<Bar>> bars_by_symbol;
        for (const auto& bar : data) {
            // Validate essential fields
            if (bar.symbol.empty()) {
                return make_error<void>(ErrorCode::INVALID_DATA, "Bar has empty symbol",
                                        "TrendFollowingStrategy");
            }

            if (bar.timestamp == Timestamp{}) {
                return make_error<void>(ErrorCode::INVALID_DATA, "Bar has invalid timestamp",
                                        "TrendFollowingStrategy");
            }

            if (bar.open <= 0.0 || bar.high <= 0.0 || bar.high < bar.low || bar.low <= 0.0 ||
                bar.close <= 0.0 || bar.volume < 0.0) {
                return make_error<void>(ErrorCode::INVALID_DATA,
                                        "Invalid bar data for symbol " + bar.symbol,
                                        "TrendFollowingStrategy");
            }

            // Group bars by symbol
            bars_by_symbol[bar.symbol].push_back(bar);
        }

        // Process each symbol
        for (const auto& [symbol, symbol_bars] : bars_by_symbol) {
            auto& instrument_data = instrument_data_[symbol];

            // Update price history
            for (const auto& bar : symbol_bars) {
                instrument_data.price_history.push_back(static_cast<double>(bar.close));
                
                // MEMORY FIX: Limit price history to maximum needed lookback (2520 days)
                if (instrument_data.price_history.size() > 2520) {
                    instrument_data.price_history.erase(instrument_data.price_history.begin());
                }
            }

            // Wait for enough data before processing
            if (instrument_data.price_history.size() < max_window) {
                if (instrument_data.price_history.size() % 50 == 0) {
                    INFO("Waiting for enough data for symbol " + symbol + " (" +
                         std::to_string(instrument_data.price_history.size()) + " of " +
                         std::to_string(max_window) + ")");
                }
                continue;
            }

            // Get price history for the symbol (limit to last 1000 days for calculations)
            const auto& full_prices = instrument_data.price_history;
            std::vector<double> prices;
            if (full_prices.size() > 1000) {
                prices.assign(full_prices.end() - 1000, full_prices.end());
            } else {
                prices = full_prices;
            }

            // Calculate volatility
            std::vector<double> volatility;
            try {
                volatility = blended_ewma_stddev(prices, trend_config_.vol_lookback_short);
                if (volatility.empty()) {
                    // If volatility calculation fails, use a default value
                    volatility.resize(prices.size(), 0.01);
                    WARN("Using default volatility for " + symbol + " due to calculation issues");
                }
            } catch (const std::exception& e) {
                WARN("Volatility calculation exception for " + symbol + ": " + e.what());
                volatility.resize(prices.size(), 0.01);
            }

            // DEBUG: Print volatility values
            if (!volatility.empty()) {
                DEBUG("Symbol " + symbol +
                      " volatility: last=" + std::to_string(volatility.back()) + ", min=" +
                      std::to_string(*std::min_element(volatility.begin(), volatility.end())) +
                      ", max=" +
                      std::to_string(*std::max_element(volatility.begin(), volatility.end())));
            }

            if (volatility.size() > MAX_HISTORY_SIZE) {
                ERROR("Volatility history for " + symbol + " exceeds max size.");
            }
            // Save volatility history with memory management
            instrument_data.volatility_history = volatility;
            instrument_data.current_volatility = volatility.back();
            
            // MEMORY FIX: Limit volatility history to prevent unbounded growth
            if (instrument_data.volatility_history.size() > 2520) {
                instrument_data.volatility_history.erase(
                    instrument_data.volatility_history.begin(),
                    instrument_data.volatility_history.begin() + 
                    (instrument_data.volatility_history.size() - 2520)
                );
            }

            // Get raw combined forecast
            std::vector<double> raw_forecasts;
            try {
                raw_forecasts = get_raw_combined_forecast(prices);
                if (raw_forecasts.empty()) {
                    WARN("Empty raw forecast for " + symbol);
                    // Resize to avoid issues
                    raw_forecasts.resize(prices.size(), 0.0);
                }
            } catch (const std::exception& e) {
                WARN("Forecast calculation exception for " + symbol + ": " + e.what());
                // Resize to avoid issues
                raw_forecasts.resize(prices.size(), 0.0);
            }

            // DEBUG: Print raw forecast values
            if (!raw_forecasts.empty()) {
                DEBUG(
                    "Symbol " + symbol +
                    " raw forecast: last=" + std::to_string(raw_forecasts.back()) + ", min=" +
                    std::to_string(*std::min_element(raw_forecasts.begin(), raw_forecasts.end())) +
                    ", max=" +
                    std::to_string(*std::max_element(raw_forecasts.begin(), raw_forecasts.end())));
            }

            instrument_data.raw_forecasts = raw_forecasts;

            // Get scaled forecast
            std::vector<double> scaled_forecasts;
            try {
                scaled_forecasts = get_scaled_combined_forecast(raw_forecasts);
                if (scaled_forecasts.empty()) {
                    WARN("Empty scaled forecast for " + symbol);
                    scaled_forecasts.resize(raw_forecasts.size(), 0.0);
                }
            } catch (const std::exception& e) {
                WARN("Scaled forecast exception for " + symbol + ": " + e.what());
                scaled_forecasts.resize(raw_forecasts.size(), 0.0);
            }

            instrument_data.scaled_forecasts = scaled_forecasts;
            instrument_data.current_forecast = scaled_forecasts.back();

            // Load instruments if not yet cached
            if (instrument_data.contract_size == 1.0) {
                // Look up instrument and cache contract size and weight
                std::string lookup_symbol = symbol;
                if (symbol.find(".v.") != std::string::npos) {
                    lookup_symbol = symbol.substr(0, symbol.find(".v."));
                }

                if (registry_ && registry_->has_instrument(lookup_symbol)) {
                    auto instrument = registry_->get_instrument(lookup_symbol);
                    if (instrument) {
                        instrument_data.contract_size = instrument->get_multiplier();
                    } else {
                        WARN("Instrument not found in registry for " + symbol);
                    }

                    if (lookup_symbol == "ES") {
                        lookup_symbol = "MES";
                    } else if (lookup_symbol == "NQ") {
                        lookup_symbol = "MNQ";
                    } else if (lookup_symbol == "YM") {
                        lookup_symbol = "MYM";
                    }

                    // Get weight
                    if (get_weights().count(lookup_symbol) > 0) {
                        instrument_data.weight = get_weights().at(lookup_symbol);
                    } else {
                        WARN("Weight not found for " + symbol);
                    }
                }
            }

            // Calculate position using the most recent forecast value
            double raw_position = 0.0;
            try {
                // Get latest forecast and volatility
                double latest_forecast = instrument_data.current_forecast;
                double latest_volatility = instrument_data.current_volatility;

                // Guard against extreme values or NaN
                if (std::isnan(latest_forecast) || std::isinf(latest_forecast)) {
                    WARN("Invalid forecast value for " + symbol + ", using 0.0");

                    // Use 0.0 to avoid extreme positions
                    latest_forecast = 0.0;
                }

                if (std::isnan(latest_volatility) || std::isinf(latest_volatility) ||
                    latest_volatility <= 0.0) {
                    WARN("Invalid volatility value for " + symbol + ", using default 0.01");

                    // Use a default value to avoid extreme positions
                    latest_volatility = 0.2;
                }

                // Get latest price
                double latest_price = prices.back();

                raw_position =
                    calculate_position(symbol, latest_forecast, latest_price, latest_volatility);
            } catch (const std::exception& e) {
                WARN("Position calculation exception for " + symbol + ": " + e.what());
                raw_position = 0.0;
            }

            instrument_data.raw_position = raw_position;

            // Apply buffering if enabled with error handling
            double final_position = 0.0;
            try {
                double latest_price = prices.back();
                if (trend_config_.use_position_buffering) {
                    final_position = apply_position_buffer(symbol, raw_position, latest_price,
                                                           instrument_data.current_volatility);
                } else {
                    final_position = raw_position;
                }

                std::string buffering_status = "";
                if (!trend_config_.use_position_buffering) {
                    buffering_status = " before dynamic optimization: ";
                } else {
                    buffering_status = " with rounding: ";
                }

                DEBUG("Symbol " + symbol + " final position" + buffering_status +
                      std::to_string(final_position));

                // Ensure position is reasonable (not NaN or infinite)
                if (std::isnan(final_position) || std::isinf(final_position)) {
                    WARN("Invalid final position for " + symbol + ", using previous position");

                    // Use previous position or zero
                    auto pos_it = positions_.find(symbol);
                    final_position = (pos_it != positions_.end())
                                         ? static_cast<double>(pos_it->second.quantity)
                                         : 0.0;
                }
            } catch (const std::exception& e) {
                WARN("Position buffering exception for " + symbol + ": " + e.what());

                // Use previous position or zero as fallback
                auto pos_it = positions_.find(symbol);
                final_position = (pos_it != positions_.end())
                                     ? static_cast<double>(pos_it->second.quantity)
                                     : 0.0;
            }

            instrument_data.final_position = final_position;

            // Save forecast with error handling
            auto signal_result =
                on_signal(symbol, scaled_forecasts.empty() ? 0.0 : scaled_forecasts.back());
            if (signal_result.is_error()) {
                WARN("Failed to save signal for " + symbol + ": " + signal_result.error()->what());
                // Continue processing despite signal save failure
            }

            // Update position with proper PnL calculation
            Position pos;
            pos.symbol = symbol;
            pos.quantity = final_position;
            pos.last_update = symbol_bars.back().timestamp;
            
            // Get current market price
            double current_price = static_cast<double>(symbol_bars.back().close);
            
            // Get previous position for PnL calculation from loaded previous day positions
            auto prev_pos_it = previous_positions_.find(symbol);
            double previous_quantity = 0.0;
            double previous_avg_price = current_price;
            double previous_realized_pnl = 0.0;
            
            if (prev_pos_it != previous_positions_.end()) {
                previous_quantity = static_cast<double>(prev_pos_it->second.quantity);
                previous_avg_price = static_cast<double>(prev_pos_it->second.average_price);
                previous_realized_pnl = static_cast<double>(prev_pos_it->second.realized_pnl);
            }
            
            // Calculate realized PnL from position changes
            double position_realized_pnl = 0.0;
            double new_avg_price = current_price;
            
            if (previous_quantity != 0.0 && final_position != 0.0) {
                // Position size changed - calculate realized PnL for the difference
                double qty_change = final_position - previous_quantity;
                if (std::abs(qty_change) > 1e-6) {
                    // Check if position is being reduced (same sign, smaller magnitude)
                    if ((previous_quantity > 0 && final_position > 0 && final_position < previous_quantity) ||
                        (previous_quantity < 0 && final_position < 0 && std::abs(final_position) < std::abs(previous_quantity))) {
                        // Position reduced - realize PnL on the closed portion
                        double closed_qty = previous_quantity - final_position;
                        double point_value = get_point_value_multiplier(symbol);
                        position_realized_pnl = closed_qty * (current_price - previous_avg_price) * point_value;
                        // Keep the same average price for remaining position
                        new_avg_price = previous_avg_price;
                    } else if ((previous_quantity > 0 && final_position > 0 && final_position > previous_quantity) ||
                               (previous_quantity < 0 && final_position < 0 && std::abs(final_position) > std::abs(previous_quantity))) {
                        // Position increased in same direction - calculate new weighted average price
                        double additional_qty = final_position - previous_quantity;
                        double total_cost = previous_quantity * previous_avg_price + additional_qty * current_price;
                        new_avg_price = total_cost / final_position;
                        position_realized_pnl = 0.0; // No PnL realized when increasing position
                    } else if ((previous_quantity > 0 && final_position < 0) || (previous_quantity < 0 && final_position > 0)) {
                        // Position reversal - close old position and open new one
                        double point_value = get_point_value_multiplier(symbol);
                        position_realized_pnl = previous_quantity * (current_price - previous_avg_price) * point_value;
                        new_avg_price = current_price;
                    }
                } else {
                    // No significant quantity change - keep same average price
                    new_avg_price = previous_avg_price;
                }
            } else if (previous_quantity != 0.0 && final_position == 0.0) {
                // Position completely closed - realize all PnL
                double point_value = get_point_value_multiplier(symbol);
                position_realized_pnl = previous_quantity * (current_price - previous_avg_price) * point_value;
                new_avg_price = current_price;
            } else if (previous_quantity == 0.0 && final_position != 0.0) {
                // New position - no realized PnL, use current price as average
                new_avg_price = current_price;
                position_realized_pnl = 0.0;
            } else {
                // No position change
                new_avg_price = previous_avg_price;
                position_realized_pnl = 0.0;
            }
            
            // Set position values
            pos.average_price = new_avg_price;
            pos.realized_pnl = Decimal(previous_realized_pnl + position_realized_pnl);
            
            // Add position-specific realized PnL to accounting system
            if (std::abs(position_realized_pnl) > 1e-6) {
                pnl_accounting_.add_realized_pnl(position_realized_pnl);
            }
            
            // For futures (marked-to-market): calculate mark-to-market PnL
            // In REALIZED_ONLY accounting, this becomes realized PnL due to daily settlement
            double mark_to_market_pnl = 0.0;
            if (std::abs(final_position) > 1e-6) {
                // For futures: use point value multiplier, not full contract size
                double point_value = get_point_value_multiplier(symbol);
                mark_to_market_pnl = final_position * (current_price - new_avg_price) * point_value;
            }
            
            // For futures with REALIZED_ONLY accounting: all PnL is realized due to daily settlement
            if (pnl_accounting_.method == PnLAccountingMethod::REALIZED_ONLY) {
                pos.realized_pnl += Decimal(mark_to_market_pnl);
                pos.unrealized_pnl = Decimal(0.0);  // Always zero for futures
                pnl_accounting_.add_realized_pnl(mark_to_market_pnl);
            } else {
                // For other accounting methods, use traditional unrealized PnL
                pos.unrealized_pnl = Decimal(mark_to_market_pnl);
                pnl_accounting_.add_unrealized_pnl(mark_to_market_pnl);
            }
            
            INFO("Position update for " + symbol + ": prev_qty=" + std::to_string(previous_quantity) + 
                  " new_qty=" + std::to_string(final_position) + 
                  " prev_avg=" + std::to_string(previous_avg_price) + 
                  " new_avg=" + std::to_string(new_avg_price) + 
                  " current_price=" + std::to_string(current_price) + 
                  " realized_pnl_change=" + std::to_string(position_realized_pnl) + 
                  " total_realized_pnl=" + std::to_string(static_cast<double>(pos.realized_pnl)) + 
                  " mark_to_market_pnl=" + std::to_string(mark_to_market_pnl));

            auto pos_result = update_position(symbol, pos);
            if (pos_result.is_error()) {
                WARN("Failed to update position for " + symbol + ": " + pos_result.error()->what());
                // Continue processing despite position update failure
            }

            instrument_data.last_update = symbol_bars.back().timestamp;
        }

        return Result<void>();

    } catch (const std::exception& e) {
        ERROR("Error processing data in TrendFollowingStrategy: " + std::string(e.what()));
        return make_error<void>(ErrorCode::STRATEGY_ERROR,
                                std::string("Error processing data: ") + e.what(),
                                "TrendFollowingStrategy");
    }
}

std::vector<double> TrendFollowingStrategy::calculate_ewma(const std::vector<double>& prices,
                                                           int window) const {
    std::vector<double> ewma(prices.size(), 0.0);
    if (prices.empty() || window <= 0) {
        return ewma;
    }
    double lambda = 2.0 / (window + 1);
    ewma[0] = prices[0];

    for (size_t i = 1; i < prices.size(); ++i) {
        ewma[i] = lambda * prices[i] + (1 - lambda) * ewma[i - 1];
    }
    return ewma;
}

std::vector<double> TrendFollowingStrategy::ewma_standard_deviation(
    const std::vector<double>& prices, int window) const {
    // Validation
    if (prices.empty() || window <= 0) {
        return std::vector<double>(1, 0.01);  // Return default value
    }

    if (prices.size() < 2) {
        return std::vector<double>(prices.size(), 0.01);  // Return default value
    }

    // Calculate returns with safety checks
    std::vector<double> returns(prices.size() - 1, 0.0);
    for (size_t i = 1; i < prices.size(); ++i) {
        // Avoid division by zero or negative prices
        if (prices[i - 1] <= 0.0 || prices[i] <= 0.0) {
            returns[i - 1] = 0.0;  // Use a neutral return
        } else {
            returns[i - 1] = std::log(prices[i] / prices[i - 1]);

            // Check for NaN or Inf
            if (std::isnan(returns[i - 1]) || std::isinf(returns[i - 1])) {
                returns[i - 1] = 0.0;  // Use a neutral return
            }
        }
    }

    std::vector<double> ewma_stddev(returns.size(), 0.0);
    std::vector<double> ewma_mean(returns.size(), 0.0);
    std::vector<double> ewma_variance(returns.size(), 0.0);

    double lambda = 2.0 / (window + 1);                // Compute lambda
    ewma_mean[0] = returns[0];                         // Initialize EWMA mean
    ewma_variance[0] = returns[0] * returns[0] * 0.1;  // Initial variance is zero

    for (size_t t = 1; t < returns.size(); ++t) {
        // Update EWMA mean
        ewma_mean[t] = lambda * returns[t] + (1 - lambda) * ewma_mean[t - 1];

        // Calculate deviation
        double deviation = returns[t] - ewma_mean[t];
        if (std::isnan(deviation) || std::isinf(deviation)) {
            deviation = 0.0;  // Use a neutral value
        }

        // Update EWMA variance
        ewma_variance[t] = lambda * (deviation * deviation) + (1 - lambda) * ewma_variance[t - 1];

        // Ensure variance is positive
        ewma_variance[t] = std::max(0.000001, ewma_variance[t]);

        ewma_stddev[t] = std::sqrt(ewma_variance[t]);

        // Annualize the standard deviation
        ewma_stddev[t] *= 16.0;  // Multiply by sqrt(256) for 256 trading days

        // Final safety check - ensure stddev is positive
        if (ewma_stddev[t] <= 0.0 || std::isnan(ewma_stddev[t]) || std::isinf(ewma_stddev[t])) {
            ewma_stddev[t] = 0.005;  // Use a small, positive default
        } else if (ewma_stddev[t] > 5.0) {
            ewma_stddev[t] = 5.0;  // Cap at 500%
        }
    }

    // Handle first element
    ewma_stddev[0] = ewma_stddev[1];

    // Add a final element to match the size of the price vector if needed
    // (returns vector is one element shorter than prices)
    std::vector<double> result(prices.size(), 0.0);
    for (size_t i = 0; i < ewma_stddev.size(); ++i) {
        result[i + 1] = ewma_stddev[i];
    }
    result[0] = result[1];  // Copy first valid value

    return ewma_stddev;
}

double TrendFollowingStrategy::compute_long_term_avg(const std::vector<double>& history,
                                                     size_t max_history) const {
    if (history.empty())
        return 0.001;  // Return a small non-zero value instead of 0.0

    size_t start_index = history.size() > max_history ? history.size() - max_history : 0;
    double sum = std::accumulate(history.begin() + start_index, history.end(), 0.0);

    // Ensure we don't divide by zero
    size_t count = history.size() - start_index;
    if (count == 0)
        return 0.001;

    double result = sum / count;

    // Sanity check on the result
    if (std::isnan(result) || std::isinf(result) || result <= 0.0) {
        return 0.001;  // Return a safe default
    }

    return result;
}

std::vector<double> TrendFollowingStrategy::blended_ewma_stddev(const std::vector<double>& prices,
                                                                int window, double weight_short,
                                                                double weight_long,
                                                                size_t max_history) const {
    if (prices.empty() || window <= 0) {
        WARN("Empty price data or invalid window for blended stddev calculation");
        return std::vector<double>(1, 0.01);  // Return default value
    }

    if (prices.size() < 2) {
        WARN("Not enough price data for blended stddev calculation");
        // Return a vector of small values
        return std::vector<double>(prices.size(), 0.01);
    }

    // Calculate EWMA standard deviation with error handling
    std::vector<double> ewma_stddev;
    try {
        ewma_stddev = ewma_standard_deviation(prices, window);
        if (ewma_stddev.empty()) {
            return std::vector<double>(prices.size(), 0.01);  // Default value
        }
    } catch (const std::exception& e) {
        ERROR("Exception in blended_ewma_stddev: " + std::string(e.what()));
        return std::vector<double>(prices.size(), 0.01);  // Default value
    }

    // Ensure ewma_stddev has the same size as prices
    if (ewma_stddev.size() != prices.size()) {
        // Adjust size by either copying the last value or truncating
        if (ewma_stddev.size() < prices.size()) {
            double last_value = ewma_stddev.empty() ? 0.01 : ewma_stddev.back();
            ewma_stddev.resize(prices.size(), last_value);
        } else {
            ewma_stddev.resize(prices.size());
        }
    }

    std::vector<double> blended_stddev(prices.size(), 0.0);
    std::vector<double> history;  // Stores past short-term EWMA standard deviations

    // Apply floor to avoid division by zero or very small values
    const double MIN_STDDEV = 0.005;

    for (size_t t = 0; t < prices.size(); ++t) {
        // Ensure we have valid stddev value before pushing to history
        double valid_stddev = std::max(MIN_STDDEV, ewma_stddev[t]);
        if (!std::isnan(valid_stddev) && !std::isinf(valid_stddev)) {
            history.push_back(valid_stddev);  // Store the short-term EWMA standard deviation
        } else {
            history.push_back(MIN_STDDEV);  // Store a safe value
        }

        double long_term_avg = compute_long_term_avg(history, max_history);
        // Ensure long term average is also valid
        long_term_avg = std::max(MIN_STDDEV, long_term_avg);

        blended_stddev[t] = weight_short * valid_stddev + weight_long * long_term_avg;

        // Final safety check
        if (blended_stddev[t] <= 0.0 || std::isnan(blended_stddev[t]) ||
            std::isinf(blended_stddev[t])) {
            blended_stddev[t] = MIN_STDDEV;  // Avoid division by zero
        }
    }

    return blended_stddev;
}

std::vector<double> TrendFollowingStrategy::get_raw_forecast(const std::vector<double>& prices,
                                                             int short_window,
                                                             int long_window) const {
    // Validation
    if (prices.size() < std::max(short_window, long_window)) {
        ERROR("Not enough price data for raw forecast");
        return std::vector<double>(prices.size(), 0.0);  // Return neutral forecast
    }

    // Calculate EWMAs with error handling
    std::vector<double> short_ema;
    std::vector<double> long_ema;

    try {
        short_ema = calculate_ewma(prices, short_window);
        long_ema = calculate_ewma(prices, long_window);
    } catch (const std::exception& e) {
        ERROR("Exception in get_raw_forecast: " + std::string(e.what()));
        return std::vector<double>(prices.size(), 0.0);  // Return neutral forecast
    }

    // Check if either EMA calculation failed
    if (short_ema.empty() || long_ema.empty()) {
        return std::vector<double>(prices.size(), 0.0);  // Return neutral forecast
    }

    // Get volatility with error handling
    std::vector<double> blended_stddev;
    double vol_multiplier = 1.0;  // Default value

    try {
        blended_stddev = blended_ewma_stddev(prices, trend_config_.vol_lookback_short);

        // Only calculate vol_multiplier if we have sufficient data
        if (prices.size() >= 252) {
            vol_multiplier = calculate_vol_regime_multiplier(prices, blended_stddev);
            // Ensure vol_multiplier is valid
            if (std::isnan(vol_multiplier) || std::isinf(vol_multiplier)) {
                vol_multiplier = 1.0;
            }
        }
    } catch (const std::exception& e) {
        ERROR("Exception in get_raw_forecast: " + std::string(e.what()));
        // If volatility calculation fails, use a default value
        blended_stddev.resize(prices.size(), 0.01);
    }

    // Check volatility calculation
    if (blended_stddev.empty()) {
        blended_stddev.resize(prices.size(), 0.01);  // Use default value
    }

    // Generate forecast with safety checks
    std::vector<double> raw_forecast(prices.size(), 0.0);
    for (size_t i = 0; i < prices.size(); ++i) {
        // Safety check for accessing elements
        if (i < short_ema.size() && i < long_ema.size() && i < blended_stddev.size() &&
            i < prices.size()) {
            // Ensure no division by zero
            double volatility = blended_stddev[i];
            if (volatility <= 0.0)
                volatility = 0.01;  // Minimum value

            double price = prices[i];
            if (price <= 0.0)
                price = 1.0;  // Minimum value

            raw_forecast[i] = (short_ema[i] - long_ema[i]) / (price * volatility / 16);

            // Apply regime multiplier
            raw_forecast[i] *= vol_multiplier;
        }
    }

    return raw_forecast;
}

std::vector<double> TrendFollowingStrategy::get_scaled_forecast(
    const std::vector<double>& raw_forecasts, const std::vector<double>& blended_stddev) const {
    if (raw_forecasts.empty() || blended_stddev.empty())
        return {};

    double abs_sum = get_abs_value(raw_forecasts);
    double abs_avg = abs_sum / raw_forecasts.size();

    std::vector<double> scaled_forecasts(raw_forecasts.size(), 0.0);
    for (size_t i = 0; i < raw_forecasts.size(); ++i) {
        scaled_forecasts[i] = 10.0 * raw_forecasts[i] / abs_avg;
        scaled_forecasts[i] = std::max(-20.0, std::min(20.0, scaled_forecasts[i]));
    }

    return scaled_forecasts;
}

std::vector<double> TrendFollowingStrategy::get_raw_combined_forecast(
    const std::vector<double>& prices) const {
    if (prices.size() < 2) {
        WARN("Not enough price data for combined forecast");
        return std::vector<double>(prices.size(), 0.0);  // Return neutral forecast
    }
    if (trend_config_.ema_windows.empty()) {
        WARN("No EMA windows specified for combined forecast");
        return std::vector<double>(prices.size(), 0.0);  // Return neutral forecast
    }

    // Initialize combined forecast and count valid window pairs
    std::vector<double> combined_forecast(prices.size(), 0.0);
    int valid_window_pairs = 0;

    // Iterate through each window pair
    for (const auto& window_pair : trend_config_.ema_windows) {
        try {
            // Calculate raw forecast for this window pair
            std::vector<double> raw_forecast =
                get_raw_forecast(prices, window_pair.first, window_pair.second);
            // Skip if invalid
            if (raw_forecast.empty() || raw_forecast.size() != prices.size()) {
                WARN("Invalid raw forecast for window pair (" + std::to_string(window_pair.first) +
                     ", " + std::to_string(window_pair.second) + "), skipping");
                continue;
            }
            // Get volatility for scaling
            std::vector<double> blended_stddev;
            try {
                blended_stddev = blended_ewma_stddev(prices, window_pair.first);

                // Check if volatility calculation failed
                if (blended_stddev.empty() || blended_stddev.size() != prices.size()) {
                    WARN("Invalid volatility for window pair (" +
                         std::to_string(window_pair.first) + ", " +
                         std::to_string(window_pair.second) + "), using default");
                    blended_stddev.resize(prices.size(), 0.01);
                }
            } catch (const std::exception& e) {
                ERROR("Exception in get_raw_combined_forecast: " + std::string(e.what()));
                blended_stddev.resize(prices.size(), 0.01);
            }
            // Get scaled forecast
            std::vector<double> scaled_forecast = get_scaled_forecast(raw_forecast, blended_stddev);

            // Combine forecasts
            for (size_t i = 0; i < combined_forecast.size(); ++i) {
                combined_forecast[i] += scaled_forecast[i];
            }
            valid_window_pairs++;
        } catch (const std::exception& e) {
            ERROR("Exception in get_raw_combined_forecast: " + std::string(e.what()));
        }
    }

    // Normalize combined forecast by the number of valid window pairs
    if (valid_window_pairs > 0) {
        for (size_t i = 0; i < combined_forecast.size(); ++i) {
            combined_forecast[i] /= valid_window_pairs;
        }
    } else {
        combined_forecast.clear();
    }

    return combined_forecast;
}

std::vector<double> TrendFollowingStrategy::get_scaled_combined_forecast(
    const std::vector<double>& raw_combined_forecast) const {
    if (raw_combined_forecast.empty())
        return {};

    // Get FDM from trend_config_ based on the number of rules
    size_t num_rules = trend_config_.ema_windows.size();
    double fdm = 1.0;  // Default if not found
    for (const auto& fdm_pair : trend_config_.fdm) {
        if (fdm_pair.first == num_rules) {
            fdm = fdm_pair.second;
            break;
        }
    }

    // Multiply raw combined forecast by FDM and scale to [-20, 20]
    std::vector<double> scaled_combined_forecast(raw_combined_forecast.size(), 0.0);
    for (size_t i = 0; i < raw_combined_forecast.size(); ++i) {
        scaled_combined_forecast[i] = raw_combined_forecast[i] * fdm;
        scaled_combined_forecast[i] = std::max(-20.0, std::min(20.0, scaled_combined_forecast[i]));
    }

    return scaled_combined_forecast;
}

double TrendFollowingStrategy::get_abs_value(const std::vector<double>& values) const {
    return std::accumulate(values.begin(), values.end(), 0.0,
                           [](double acc, double val) { return acc + std::abs(val); });
}

std::unordered_map<std::string, double> TrendFollowingStrategy::get_weights() const {
    auto metadata_result = db_->get_contract_metadata();
    if (!metadata_result.is_ok()) {
        ERROR(std::string("Failed to get contract metadata: ")
                  .append(metadata_result.error()->what()));
        return {};
    }

    auto metadata = metadata_result.value();
    int sector_idx = metadata->schema()->GetFieldIndex("Sector");
    int symbol_idx = metadata->schema()->GetFieldIndex("Databento Symbol");

    if (sector_idx == -1 || symbol_idx == -1) {
        ERROR("Sector or Databento Symbol column not found in metadata schema");
        return {};
    }

    auto sector_col = metadata->column(sector_idx);
    auto symbol_col = metadata->column(symbol_idx);

    // Build map of sector -> list of symbols
    std::unordered_map<std::string, std::vector<std::string>> sector_to_symbols;

    for (int chunk_idx = 0; chunk_idx < sector_col->num_chunks(); ++chunk_idx) {
        auto sector_array =
            std::static_pointer_cast<arrow::StringArray>(sector_col->chunk(chunk_idx));
        auto symbol_array =
            std::static_pointer_cast<arrow::StringArray>(symbol_col->chunk(chunk_idx));

        int64_t num_rows = sector_array->length();
        for (int64_t i = 0; i < num_rows; ++i) {
            if (!sector_array->IsNull(i) && !symbol_array->IsNull(i)) {
                std::string sector = sector_array->GetString(i);
                std::string symbol = symbol_array->GetString(i);
                sector_to_symbols[sector].push_back(symbol);
            }
        }
    }

    // Compute weights
    std::unordered_map<std::string, double> symbol_weights;
    int total_sectors = static_cast<int>(sector_to_symbols.size());

    if (total_sectors == 0)
        return symbol_weights;

    double sector_weight = 1.0 / total_sectors;

    for (const auto& [sector, symbols] : sector_to_symbols) {
        int num_symbols = static_cast<int>(symbols.size());
        if (num_symbols == 0)
            continue;

        double per_symbol_weight = sector_weight / num_symbols;
        for (const auto& symbol : symbols) {
            symbol_weights[symbol] = per_symbol_weight;
        }
    }

    return symbol_weights;
}

double TrendFollowingStrategy::calculate_position(const std::string& symbol, double forecast,
                                                  double price, double volatility) const {
    // Validation
    if (std::isnan(forecast) || std::isinf(forecast) || std::abs(forecast) > 20.0) {
        WARN("Invalid forecast in position calculation for " + symbol + ", using 0.0");
        return 0.0;
    }

    if (std::isnan(price) || price <= 0.0) {
        WARN("Invalid price in position calculation for " + symbol + ": " + std::to_string(price));
        // Try to find last valid price
        auto it = price_history_.find(symbol);
        if (it != price_history_.end() && !it->second.empty()) {
            price = it->second.back();
        } else {
            WARN("Cannot find valid price for " + symbol + ", using 1.0");
            price = 1.0;  // Use safe default
        }
    }

    if (std::isnan(volatility) || volatility <= 0.0) {
        WARN("Invalid volatility in position calculation for " + symbol + ": " +
             std::to_string(volatility));
        volatility = 0.01;  // Use safe default
    }

    auto it = instrument_data_.find(symbol);
    if (it != instrument_data_.end()) {
        const auto& data = it->second;

        // Use cached values
        double contract_size = data.contract_size;
        double weight = data.weight;
        double capital = std::max(1000.0, config_.capital_allocation);
        double idm = std::max(0.1, trend_config_.idm);
        double risk_target = std::max(0.01, std::min(0.5, trend_config_.risk_target));
        double fx_rate = std::max(0.1, trend_config_.fx_rate);

        // Apply minimum value to volatility to avoid division by very small values
        volatility = std::clamp(volatility, 0.01, 1.0);

        DEBUG("Calculating position for " + symbol + " with forecast=" + std::to_string(forecast) +
              ", price=" + std::to_string(price) + ", volatility=" + std::to_string(volatility) +
              ", contract_size=" + std::to_string(contract_size) +
              ", weight=" + std::to_string(weight) + ", capital=" + std::to_string(capital) +
              ", idm=" + std::to_string(idm) + ", risk_target=" + std::to_string(risk_target) +
              ", fx_rate=" + std::to_string(fx_rate));

        // Calculate position using volatility targeting formula with safeguards
        double denominator = 10.0 * contract_size * price * fx_rate * volatility;
        denominator = std::max(denominator, 1.0);  // Prevent division by zero or tiny values

        double position = (forecast * capital * weight * idm * risk_target) / denominator;

        // Handle potential NaN or Inf results
        if (std::isnan(position) || std::isinf(position)) {
            WARN("Invalid position calculation result for " + symbol + ": " +
                 std::to_string(position));
            position = 0.0;  // Use neutral position
        }

        // Apply position limits as a safeguard
        double position_limit = 1000.0;
        if (config_.position_limits.count(symbol) > 0) {
            position_limit = config_.position_limits.at(symbol);
        }

        double final_position = std::clamp(position, -position_limit, position_limit);
        if (abs(final_position) >= 1000.0) {
            WARN("Position limit reached for " + symbol + ": " + std::to_string(final_position));
        }

        INFO("Final position: " + std::to_string(final_position));

        return final_position;
    } else {
        ERROR("No instrument data found for " + symbol);
        return 0.0;  // Use neutral position
    }
}

double TrendFollowingStrategy::apply_position_buffer(const std::string& symbol, double raw_position,
                                                     double price, double volatility) const {
    if (!trend_config_.use_position_buffering) {
        return raw_position;
    }

    // Validation
    if (std::isnan(raw_position) || std::isinf(raw_position)) {
        WARN("Invalid raw_position for " + symbol + ", returning 0");
        return 0.0;
    }

    if (std::isnan(price) || price <= 0.0) {
        WARN("Invalid price for " + symbol + ", returning 0");
        return 0.0;
    }

    if (std::isnan(volatility) || std::isinf(volatility) || volatility <= 0.0) {
        WARN("Invalid volatility for " + symbol + ", returning 0");
        return 0.0;
    }

    // Get current position with safeguards
    double current_position = 0.0;
    auto pos_it = positions_.find(symbol);
    if (pos_it != positions_.end()) {
        current_position = static_cast<double>(pos_it->second.quantity);

        // Sanity check on current position
        if (std::isnan(current_position) || std::isinf(current_position)) {
            current_position = 0.0;
        }
    }

    double weight = std::max(0.0, trend_config_.weight);

    // Get contract size from instrument registry (use cached value if available)
    double contract_size = 1.0;
    auto inst_data_it = instrument_data_.find(symbol);
    if (inst_data_it != instrument_data_.end()) {
        contract_size = inst_data_it->second.contract_size;
    } else {
        // Fallback: lookup from registry
        try {
            std::string lookup_symbol = symbol;
            if (symbol.find(".v.") != std::string::npos) {
                lookup_symbol = symbol.substr(0, symbol.find(".v."));
            }
            if (symbol.find(".c.") != std::string::npos) {
                lookup_symbol = symbol.substr(0, symbol.find(".c."));
            }

            if (registry_ && registry_->has_instrument(lookup_symbol)) {
                auto instrument = registry_->get_instrument(lookup_symbol);
                if (instrument) {
                    contract_size = instrument->get_multiplier();
                }
            }
        } catch (const std::exception& e) {
            WARN("Failed to get contract size for " + symbol + ": " + std::string(e.what()));
            contract_size = 1.0;
        }
    }

    // IMPLEMENTATION: Calculate buffer width using Carver's formula
    // buffer_width = 0.1 * capital * IDM * weight * risk_target /
    //                (contract_size * price * fx_rate * volatility)
    double buffer_width = 0.1 * config_.capital_allocation * trend_config_.idm *
                          trend_config_.risk_target * weight /
                          (contract_size * price * trend_config_.fx_rate * volatility);

    // PURE CARVER APPROACH - No clamping applied
    // This allows the buffer to scale naturally with position size and market conditions
    // as originally intended in the methodology
    DEBUG("Buffer width for " + symbol + ": " + std::to_string(buffer_width) +
          " contracts (unclamped Carver approach)");

    // Calculate buffer bounds
    double lower_buffer = raw_position - buffer_width;
    double upper_buffer = raw_position + buffer_width;

    // IMPLEMENTATION: Apply buffering logic
    // If current position is within the buffer zone [lower_buffer, upper_buffer],
    // keep the current position (no trade). Otherwise, trade to the buffer boundary.
    double new_position;
    if (current_position < lower_buffer) {
        // Current position is below the buffer zone - trade up to lower boundary
        new_position = std::round(lower_buffer);
        DEBUG("Position buffering for " + symbol + ": trading from " +
              std::to_string(current_position) + " to lower buffer " +
              std::to_string(new_position) + " (raw: " + std::to_string(raw_position) + ")");
    } else if (current_position > upper_buffer) {
        // Current position is above the buffer zone - trade down to upper boundary
        new_position = std::round(upper_buffer);
        DEBUG("Position buffering for " + symbol + ": trading from " +
              std::to_string(current_position) + " to upper buffer " +
              std::to_string(new_position) + " (raw: " + std::to_string(raw_position) + ")");
    } else {
        // Current position is within the buffer zone - no trade needed
        new_position = std::round(current_position);
        DEBUG("Position buffering for " + symbol + ": keeping current position " +
              std::to_string(new_position) + " (within buffer, raw: " +
              std::to_string(raw_position) + ")");
    }

    // Final safety check - cap positions to configured limits
    double position_limit = 1000.0;
    if (config_.position_limits.count(symbol) > 0) {
        position_limit = config_.position_limits.at(symbol);
    }

    double final_position = std::max(-position_limit, std::min(position_limit, new_position));

    if (std::abs(final_position - new_position) > 0.1) {
        WARN("Position for " + symbol + " capped by position limit: " +
             std::to_string(new_position) + " -> " + std::to_string(final_position));
    }

    return final_position;
}

double TrendFollowingStrategy::calculate_vol_regime_multiplier(
    const std::vector<double>& prices, const std::vector<double>& volatility) const {
    if (prices.size() < 252) {  // Need at least 1 year of data
        return (2.0 / 3.0);     // Default multiplier if insufficient data
    }

    // Get current blended volatility (last value in volatility vector)
    double current_vol = volatility.back();

    // Calculate lookback period for long-run average
    size_t max_lookback = 2520;  // 10 years
    size_t available_days = prices.size();
    size_t lookback = std::min(available_days, max_lookback);

    // Calculate long-run average volatility
    double sum_vol = 0.0;
    size_t count = 0;
    for (size_t i = volatility.size() - lookback; i < volatility.size(); ++i) {
        sum_vol += volatility[i];
        count++;
    }
    double avg_vol = sum_vol / count;

    // Calculate relative volatility level
    double relative_vol_level = current_vol / avg_vol;

    // Calculate quantile of relative volatility levels over lookback period
    std::vector<double> historical_rel_vol_levels;
    historical_rel_vol_levels.reserve(lookback);

    for (size_t i = volatility.size() - lookback; i < volatility.size() - 1;
         ++i) {  // Exclude current day
        historical_rel_vol_levels.push_back(volatility[i] / avg_vol);
    }

    // Sort to calculate quantile
    std::sort(historical_rel_vol_levels.begin(), historical_rel_vol_levels.end());

    // Find position of current relative volatility level in sorted historical values
    auto it = std::upper_bound(historical_rel_vol_levels.begin(), historical_rel_vol_levels.end(),
                               relative_vol_level);
    double quantile = static_cast<double>(std::distance(historical_rel_vol_levels.begin(), it)) /
                      static_cast<double>(historical_rel_vol_levels.size());

    // Calculate raw multiplier using formula: 2 - 1.5 * Q
    double raw_multiplier = 2.0 - 1.5 * quantile;

    // Apply 10-day EWMA to the multiplier
    static const size_t EWMA_DAYS = 10;
    static const double alpha = 2.0 / (EWMA_DAYS + 1.0);

    // Default to 2/3 if insufficient data for EWMA
    if (historical_rel_vol_levels.size() < EWMA_DAYS) {
        return (2.0 / 3.0);
    }

    // Calculate EWMA of multiplier
    double ewma_vol_multiplier = raw_multiplier;
    double prev_ewma_vol_multiplier = ewma_vol_multiplier;

    for (size_t i = 0; i < EWMA_DAYS - 1; ++i) {
        size_t idx = volatility.size() - EWMA_DAYS + i;
        double historical_vol = volatility[idx];
        double historical_relative_vol = historical_vol / avg_vol;

        // Find quantile for this historical point
        auto hist_it = std::upper_bound(historical_rel_vol_levels.begin(),
                                        historical_rel_vol_levels.end(), historical_relative_vol);
        double hist_Q =
            static_cast<double>(std::distance(historical_rel_vol_levels.begin(), hist_it)) /
            static_cast<double>(historical_rel_vol_levels.size());

        double hist_multiplier = 2.0 - 1.5 * hist_Q;

        // Update EWMA
        ewma_vol_multiplier = alpha * hist_multiplier + (1.0 - alpha) * prev_ewma_vol_multiplier;
        prev_ewma_vol_multiplier = ewma_vol_multiplier;
    }

    INFO("EWMA volatility multiplier: " + std::to_string(ewma_vol_multiplier) +
         " with quantile: " + std::to_string(quantile));

    return ewma_vol_multiplier;
}

double TrendFollowingStrategy::get_point_value_multiplier(const std::string& symbol) const {
    // Extract base symbol (remove .v./.c. suffix)
    std::string base_symbol = symbol;
    if (symbol.find(".v.") != std::string::npos) {
        base_symbol = symbol.substr(0, symbol.find(".v."));
    }
    if (symbol.find(".c.") != std::string::npos) {
        base_symbol = symbol.substr(0, symbol.find(".c."));
    }

    // ONLY use registry - no fallbacks allowed
    if (!registry_) {
        ERROR("CRITICAL: Instrument registry not initialized when requesting multiplier for " + symbol);
        throw std::runtime_error("Instrument registry not initialized for symbol: " + symbol);
    }

    if (!registry_->has_instrument(base_symbol)) {
        ERROR("CRITICAL: Instrument " + base_symbol + " not found in registry! Cannot continue without proper multiplier.");
        throw std::runtime_error("Missing instrument in registry: " + base_symbol +
                               ". Please ensure this instrument is loaded in the database.");
    }

    try {
        auto instrument = registry_->get_instrument(base_symbol);
        if (!instrument) {
            ERROR("CRITICAL: Null instrument returned for " + base_symbol);
            throw std::runtime_error("Null instrument for: " + base_symbol);
        }

        double multiplier = instrument->get_multiplier();
        if (multiplier <= 0) {
            ERROR("CRITICAL: Invalid multiplier " + std::to_string(multiplier) +
                  " for " + base_symbol + ". Multiplier must be positive.");
            throw std::runtime_error("Invalid multiplier (" + std::to_string(multiplier) +
                                   ") for: " + base_symbol);
        }

        DEBUG("Retrieved point value multiplier from registry for " + symbol + ": " +
              std::to_string(multiplier));
        return multiplier;
    } catch (const std::exception& e) {
        ERROR("CRITICAL: Failed to get multiplier for " + symbol + ": " + e.what() +
              ". Cannot continue without proper multiplier.");
        throw;  // Re-throw the original exception
    }
}

std::unordered_map<int, double> TrendFollowingStrategy::get_ema_values(const std::string& symbol, const std::vector<int>& windows) const {
    std::unordered_map<int, double> ema_values;

    // Get instrument data for this symbol
    auto it = instrument_data_.find(symbol);
    if (it == instrument_data_.end() || it->second.price_history.empty()) {
        // Return empty map if no data available
        return ema_values;
    }

    const auto& price_history = it->second.price_history;

    // Calculate EMA for each requested window
    for (int window : windows) {
        auto ema_series = calculate_ewma(price_history, window);
        if (!ema_series.empty()) {
            // Return the most recent EMA value
            ema_values[window] = ema_series.back();
        }
    }

    return ema_values;
}

}  // namespace trade_ngin