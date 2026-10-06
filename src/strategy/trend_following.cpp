// src/strategy/trend_following.cpp
#include "trade_ngin/strategy/trend_following.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/strategy/trend_estimator.hpp"
#include "trade_ngin/strategy/trend_estimator_record.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <numeric>
#include <set>
#include <unordered_set>

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

    // The history the strategy keeps is the estimators' window: the last W consumed bars of each
    // symbol (trend_estimator.hpp). Every estimator is recomputed from it on every call.
    trend_config_.max_history_size = trend_estimator::kWindowBars;

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

    // Every EWMAC pair is scaled by its fixed scalar; a pair without one is refused, never run
    // on a guessed scale.
    std::string unsupported;
    if (!trend_estimator::pairs_supported(trend_config_.ema_windows, &unsupported)) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "EMA window pair " + unsupported +
                                    " has no fixed forecast scalar: the pairs are (2,8), (4,16), "
                                    "(8,32), (16,64), (32,128) and (64,256)",
                                "TrendFollowingStrategy");
    }

    // The equity slow rule reads the scaled forecasts of the pairs it names: a ruled sleeve carries
    // every one of them, and a rule with symbols names at least one pair.
    if (!trend_config_.equity_slow_symbols.empty()) {
        if (trend_config_.equity_slow_pairs.empty()) {
            return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                    "The equity slow rule names symbols and no pair",
                                    "TrendFollowingStrategy");
        }
        for (const auto& rule_pair : trend_config_.equity_slow_pairs) {
            if (std::find(trend_config_.ema_windows.begin(), trend_config_.ema_windows.end(),
                          rule_pair) == trend_config_.ema_windows.end()) {
                return make_error<void>(
                    ErrorCode::INVALID_ARGUMENT,
                    "The equity slow rule names the pair (" + std::to_string(rule_pair.first) +
                        ", " + std::to_string(rule_pair.second) +
                        "), which is not one of this sleeve's EMA window pairs",
                    "TrendFollowingStrategy");
            }
        }
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
        // Initialize positions for each symbol
        for (const auto& [symbol, _] : config_.trading_params) {
            // Initialize positions with zero quantity
            Position pos;
            pos.symbol = symbol;
            pos.quantity = 0.0;
            pos.average_price = 0.0;  // Safe default - won't cause calculation errors with qty=0
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

Result<void> TrendFollowingStrategy::on_execution(const ExecutionReport& report) {
    // T-ROLLX (LOOP_SPEC v6.1 section 6.5): a ROLL leg (or a BORROW row) is not a trade of the
    // strategy; nothing is counted on it.
    if (report.execution_type != ExecutionType::STRATEGY) return Result<void>();
    // Override base class to prevent PnL corruption.
    // TrendFollowingStrategy calculates PnL in on_data() with proper point_value multiplier.
    // The base class on_execution() calculates PnL without point_value, which would corrupt
    // the realized_pnl values that were correctly calculated in on_data().

    // Only update trade count metric - don't modify positions or PnL
    std::lock_guard<std::mutex> lock(mutex_);
    metrics_.total_trades++;

    return Result<void>();
}

Result<void> TrendFollowingStrategy::seed_history(const std::vector<Bar>& bars) {
    // The estimators' history before the first bar the strategy is fed: consumed bars only, in
    // date order per symbol, kept to the window's length. It publishes nothing and sizes nothing.
    std::unordered_map<std::string, std::vector<const Bar*>> by_symbol;
    for (const auto& bar : bars) {
        if (bar.symbol.empty() || bar.timestamp == Timestamp{} || !(bar.close > 0.0)) {
            return make_error<void>(ErrorCode::INVALID_DATA,
                                    "Invalid bar in the seeded history for symbol " + bar.symbol,
                                    "TrendFollowingStrategy");
        }
        by_symbol[bar.symbol].push_back(&bar);
    }
    for (auto& [symbol, symbol_bars] : by_symbol) {
        std::stable_sort(symbol_bars.begin(), symbol_bars.end(),
                         [](const Bar* a, const Bar* b) { return a->timestamp < b->timestamp; });
        auto& instrument_data = instrument_data_[symbol];
        instrument_data.seeded_prices.clear();
        instrument_data.seeded_timestamps.clear();
        instrument_data.seeded_instrument_ids.clear();
        const size_t first = symbol_bars.size() > trend_config_.max_history_size
                                 ? symbol_bars.size() - trend_config_.max_history_size
                                 : 0;
        for (size_t i = first; i < symbol_bars.size(); ++i) {
            instrument_data.seeded_prices.push_back(static_cast<double>(symbol_bars[i]->close));
            instrument_data.seeded_timestamps.push_back(symbol_bars[i]->timestamp);
            instrument_data.seeded_instrument_ids.push_back(symbol_bars[i]->instrument_id);
        }
        // The seed is the history until bars are fed.
        instrument_data.price_history = instrument_data.seeded_prices;
        instrument_data.bar_timestamps = instrument_data.seeded_timestamps;
        instrument_data.bar_instrument_ids = instrument_data.seeded_instrument_ids;
    }
    return Result<void>();
}

Result<void> TrendFollowingStrategy::on_data(const std::vector<Bar>& data) {
    // Validate data
    if (data.empty()) {
        return Result<void>();
    }

    // Debug: Track on_data calls
    static int on_data_call_count = 0;
    ++on_data_call_count;
    if (on_data_call_count <= 5 || on_data_call_count % 1000 == 0) {
        auto ts_str = data.empty()
                          ? "N/A"
                          : std::to_string(std::chrono::system_clock::to_time_t(data[0].timestamp));
        INFO("ON_DATA #" + std::to_string(on_data_call_count) + ": called with " +
             std::to_string(data.size()) + " bars, first timestamp=" + ts_str);
    }

    // Log total on_data calls when we see the first loop iteration
    static bool logged_total = false;
    if (!logged_total && on_data_call_count > 100) {
        INFO("TOTAL ON_DATA CALLS before main loop: " + std::to_string(on_data_call_count));
        logged_total = true;
    }

    // CRITICAL FIX: Update price history BEFORE base class processing
    // This ensures price data is always updated even if leverage checks fail
    // in BaseStrategy::on_data(), preventing stuck prices in final_positions table

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

        // Process each symbol - update price history first
        for (const auto& [symbol, symbol_bars] : bars_by_symbol) {
            auto& instrument_data = instrument_data_[symbol];

            // Clear price history ONLY if processing bulk historical data (live mode)
            // In backtest mode, on_data is called daily with small batches, so we accumulate
            // Heuristic: if processing >100 bars for this symbol, it's bulk mode
            if (symbol_bars.size() > 100) {
                instrument_data.price_history.clear();
                instrument_data.bar_timestamps.clear();
                instrument_data.bar_instrument_ids.clear();
                // The history seeded before the feed (seed_history) stays in front of it: the
                // bars dated before the feed's first bar.
                const Timestamp feed_start = symbol_bars.front().timestamp;
                for (size_t i = 0; i < instrument_data.seeded_prices.size(); ++i) {
                    if (!(instrument_data.seeded_timestamps[i] < feed_start)) break;
                    instrument_data.price_history.push_back(instrument_data.seeded_prices[i]);
                    instrument_data.bar_timestamps.push_back(instrument_data.seeded_timestamps[i]);
                    instrument_data.bar_instrument_ids.push_back(instrument_data.seeded_instrument_ids[i]);
                }
            }

            // Update price history
            for (const auto& bar : symbol_bars) {
                instrument_data.price_history.push_back(static_cast<double>(bar.close));
                instrument_data.bar_timestamps.push_back(bar.timestamp);
                instrument_data.bar_instrument_ids.push_back(bar.instrument_id);

                // MEMORY FIX: Limit price history to maximum needed lookback
                if (instrument_data.price_history.size() > trend_config_.max_history_size) {
                    instrument_data.price_history.pop_front();
                    instrument_data.bar_timestamps.pop_front();
                    instrument_data.bar_instrument_ids.pop_front();
                }
            }
        }
    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::INVALID_DATA,
                                "Exception updating price history: " + std::string(e.what()),
                                "TrendFollowingStrategy");
    }

    // Call base class data processing (leverage checks, etc.)
    // If this fails due to leverage, price history is already updated above
    auto base_result = BaseStrategy::on_data(data);
    if (base_result.is_error())
        return base_result;

    // Get longest window in ema pairs
    int max_window = 0;
    for (const auto& window_pair : trend_config_.ema_windows) {
        max_window = std::max(max_window, window_pair.second);
    }

    try {
        // Retrieve bars_by_symbol again for signal processing
        std::unordered_map<std::string, std::vector<Bar>> bars_by_symbol;
        for (const auto& bar : data) {
            bars_by_symbol[bar.symbol].push_back(bar);
        }

        // Cache weights once per on_data() call to avoid repeated DB queries
        auto cached_weights = get_weights();

        // Process each symbol for signal generation
        for (const auto& [symbol, symbol_bars] : bars_by_symbol) {
            auto& instrument_data = instrument_data_[symbol];

            // Wait for enough data before processing
            if (instrument_data.price_history.size() < static_cast<size_t>(max_window)) {
                if (instrument_data.price_history.size() % 50 == 0) {
                    INFO("Waiting for enough data for symbol " + symbol + " (" +
                         std::to_string(instrument_data.price_history.size()) + " of " +
                         std::to_string(max_window) + ")");
                }
                continue;
            }

            // The estimators' window: the last W consumed bars the strategy holds for the symbol
            // (the history cap is W). Every RETURN consumer (the vol estimator, the EMAs, the
            // forecast's own vol, the attenuation) reads the back-adjusted series; every LEVEL (the
            // price the forecast and the sizing divide by) reads the raw close. Recomputed from the
            // window on every call: no level, mean or variance persists across bars or runs.
            const auto& full_prices = instrument_data.price_history;
            const std::vector<double> prices(full_prices.begin(), full_prices.end());
            const std::vector<std::string> instrument_ids(
                instrument_data.bar_instrument_ids.begin(), instrument_data.bar_instrument_ids.end());
            const roll_series::Series series = roll_series::build_series(prices, instrument_ids);

            // The annualisation: sqrt(bars a year) counted over the trailing 256 bars the estimator
            // has, or all of them when it has fewer (a series with a Sunday session row has about
            // 313 a year, not 256; the same count in every engine).
            const VolAnnualisation annualisation =
                trailing_vol_annualisation(instrument_data.bar_timestamps, prices.size());
            DEBUG("Symbol " + symbol + " vol annualisation: bars=" +
                  std::to_string(annualisation.bars) +
                  " span_days=" + std::to_string(annualisation.span_days) +
                  " bars_per_year=" + std::to_string(annualisation.bars_per_year) +
                  " factor=" + std::to_string(annualisation.factor) +
                  (annualisation.fallback ? " fallback=16" : ""));
            INFO(vol_annualisation_log_line("TrendFollowing", id_, symbol,
                                            symbol_bars.back().timestamp, annualisation));

            trend_estimator::Window window;
            window.close = series.raw;
            window.level = series.adjusted;
            // series.returns holds one entry per bar from the second (entry t - 1 is bar t's
            // return); the window indexes a return by its own bar, the first bar having none.
            window.returns.assign(prices.size(), 0.0);
            std::copy(series.returns.begin(), series.returns.end(), window.returns.begin() + 1);
            window.day.reserve(prices.size());
            // The bar's DATE as a whole day number: a loaded bar's timestamp sits some hours into
            // its date and not the same hours on every date, and the annualisation counts dates.
            for (const auto& ts : instrument_data.bar_timestamps) {
                window.day.push_back(static_cast<double>(
                    std::chrono::floor<std::chrono::days>(ts).time_since_epoch().count()));
            }
            double fdm = 1.0;  // the diversification multiplier for this number of pairs
            for (const auto& fdm_pair : trend_config_.fdm) {
                if (fdm_pair.first == static_cast<int>(trend_config_.ema_windows.size())) {
                    fdm = fdm_pair.second;
                    break;
                }
            }
            const trend_estimator::Estimate estimate =
                trend_estimator::estimate(window, trend_config_.vol_lookback_short,
                                          trend_config_.ema_windows, fdm);
            if (!estimate.valid) {
                WARN("Using default volatility for " + symbol + " due to calculation issues");
            }
            instrument_data.estimate = estimate;
            {
                constexpr size_t kOverlayBars = 300;
                const size_t bars = window.day.size();
                const size_t from = bars > kOverlayBars ? bars - kOverlayBars : 1;  // bar 0 has no return
                instrument_data.overlay_days.assign(window.day.begin() + static_cast<long>(from),
                                                    window.day.end());
                instrument_data.overlay_returns.assign(
                    window.returns.begin() + static_cast<long>(from), window.returns.end());
                constexpr size_t kOptimiserBars = 756;
                const size_t opt_from = bars > kOptimiserBars ? bars - kOptimiserBars : 0;
                instrument_data.opt_days.assign(window.day.begin() + static_cast<long>(opt_from),
                                                window.day.end());
                instrument_data.opt_closes.assign(window.close.begin() + static_cast<long>(opt_from),
                                                  window.close.end());
                instrument_data.opt_levels.assign(window.level.begin() + static_cast<long>(opt_from),
                                                  window.level.end());
            }
            instrument_data.current_volatility = estimate.valid ? estimate.sigma : 0.01;

            // DEBUG: Print volatility values
            DEBUG("Symbol " + symbol +
                  " volatility: last=" + std::to_string(instrument_data.current_volatility) +
                  ", min=" + std::to_string(estimate.valid ? estimate.sigma_min : 0.01) +
                  ", max=" + std::to_string(estimate.valid ? estimate.sigma_max : 0.01));

            // The attenuation the forecasts were multiplied by, once per pair, where the window
            // holds a year of bars
            if (prices.size() >= trend_estimator::kAttenuationMinValues) {
                for (size_t pair = 0; pair < trend_config_.ema_windows.size(); ++pair) {
                    INFO("EWMA volatility multiplier: " + std::to_string(estimate.attenuation) +
                         " with quantile: " + std::to_string(estimate.smoothed_quantile));
                }
            }

            // DEBUG: Print raw forecast values (the equal-weight mean of the scaled forecasts)
            DEBUG("Symbol " + symbol +
                  " raw forecast: last=" + std::to_string(estimate.mean_scaled) + ", min=" +
                  std::to_string(estimate.mean_scaled_min) + ", max=" +
                  std::to_string(estimate.mean_scaled_max));

            instrument_data.current_raw_forecast = estimate.mean_scaled;
            instrument_data.current_scaled_forecast = estimate.combined;
            // The equity slow rule (section 2.5, D40): on a ruled symbol a negative combined
            // forecast stands only when every slow pair the rule names is negative. The ruled
            // forecast is THE forecast from here on: the sizing, its sign and the stored signal.
            double ruled_forecast = estimate.combined;
            {
                const std::string base_symbol = symbol.substr(0, symbol.find('.'));
                if (std::find(trend_config_.equity_slow_symbols.begin(),
                              trend_config_.equity_slow_symbols.end(),
                              base_symbol) != trend_config_.equity_slow_symbols.end()) {
                    ruled_forecast = trend_estimator::equity_slow_ruled(
                        estimate.combined, estimate.scaled, trend_config_.ema_windows,
                        trend_config_.equity_slow_pairs);
                }
            }
            const bool slow_rule_zeroed = ruled_forecast != estimate.combined;
            instrument_data.slow_rule_zeroed = slow_rule_zeroed;
            instrument_data.current_forecast = ruled_forecast;

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

                    // Get weight from the per-call hoisted copy (get_weights() itself
                    // caches internally; hoisting avoids a map copy per symbol)
                    auto weight_it = cached_weights.find(lookup_symbol);
                    if (weight_it != cached_weights.end()) {
                        instrument_data.weight = weight_it->second;
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

                raw_position = calculate_position(symbol, latest_forecast, latest_price,
                                                  latest_volatility,
                                                  &instrument_data.optimal_position);
            } catch (const std::exception& e) {
                WARN("Position calculation exception for " + symbol + ": " + e.what());
                raw_position = 0.0;
            }

            instrument_data.raw_position = raw_position;
            append_trend_estimator_record(id_, core::format_utc_date(symbol_bars.back().timestamp),
                                          symbol, instrument_data.estimate,
                                          trend_config_.ema_windows,
                                          instrument_data.current_forecast,
                                          std::max(1000.0, config_.capital_allocation),
                                          instrument_data.weight, instrument_data.contract_size,
                                          prices.back(), instrument_data.optimal_position,
                                          slow_rule_zeroed);

            // The published position is N* itself (section 3.2): no buffer, no rounding.
            double final_position = raw_position;
            if (std::isnan(final_position) || std::isinf(final_position)) {
                WARN("Invalid final position for " + symbol + ", using 0.0");
                final_position = 0.0;
            }

            instrument_data.final_position = final_position;

            // Save forecast with error handling
            auto signal_result = on_signal(symbol, instrument_data.current_forecast);
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

            // Get previous position for PnL calculation from positions_: seeded via
            // seed_positions() on live first day, maintained by update_position()
            // on every prior bar.
            double previous_quantity = 0.0;
            double previous_avg_price = current_price;
            double previous_realized_pnl = 0.0;

            auto pos_it = positions_.find(symbol);
            if (pos_it != positions_.end()) {
                previous_quantity = static_cast<double>(pos_it->second.quantity);
                previous_avg_price = static_cast<double>(pos_it->second.average_price);
                previous_realized_pnl = static_cast<double>(pos_it->second.realized_pnl);
            }

            // Calculate realized PnL from position changes
            double position_realized_pnl = 0.0;
            double new_avg_price = current_price;

            if (previous_quantity != 0.0 && final_position != 0.0) {
                // Position size changed - calculate realized PnL for the difference
                double qty_change = final_position - previous_quantity;
                if (std::abs(qty_change) > 1e-6) {
                    // Check if position is being reduced (same sign, smaller magnitude)
                    if ((previous_quantity > 0 && final_position > 0 &&
                         final_position < previous_quantity) ||
                        (previous_quantity < 0 && final_position < 0 &&
                         std::abs(final_position) < std::abs(previous_quantity))) {
                        // Position reduced - realize PnL on the closed portion
                        double closed_qty = previous_quantity - final_position;
                        double point_value = get_point_value_multiplier(symbol);
                        position_realized_pnl =
                            closed_qty * (current_price - previous_avg_price) * point_value;
                        // Keep the same average price for remaining position
                        new_avg_price = previous_avg_price;
                    } else if ((previous_quantity > 0 && final_position > 0 &&
                                final_position > previous_quantity) ||
                               (previous_quantity < 0 && final_position < 0 &&
                                std::abs(final_position) > std::abs(previous_quantity))) {
                        // Position increased in same direction - calculate new weighted average
                        // price
                        double additional_qty = final_position - previous_quantity;
                        double total_cost =
                            previous_quantity * previous_avg_price + additional_qty * current_price;
                        new_avg_price = total_cost / final_position;
                        position_realized_pnl = 0.0;  // No PnL realized when increasing position
                    } else if ((previous_quantity > 0 && final_position < 0) ||
                               (previous_quantity < 0 && final_position > 0)) {
                        // Position reversal - close old position and open new one
                        double point_value = get_point_value_multiplier(symbol);
                        position_realized_pnl =
                            previous_quantity * (current_price - previous_avg_price) * point_value;
                        new_avg_price = current_price;
                    }
                } else {
                    // No significant quantity change - keep same average price
                    new_avg_price = previous_avg_price;
                }
            } else if (previous_quantity != 0.0 && final_position == 0.0) {
                // Position completely closed - realize all PnL
                double point_value = get_point_value_multiplier(symbol);
                position_realized_pnl =
                    previous_quantity * (current_price - previous_avg_price) * point_value;
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

            // For futures (marked-to-market): calculate mark-to-market PnL
            // In REALIZED_ONLY accounting, this becomes realized PnL due to daily settlement
            double mark_to_market_pnl = 0.0;
            if (std::abs(final_position) > 1e-6) {
                // For futures: use point value multiplier, not full contract size
                double point_value = get_point_value_multiplier(symbol);
                mark_to_market_pnl = final_position * (current_price - new_avg_price) * point_value;
            }

            // Calculate the daily PnL for this position
            double daily_realized_pnl = position_realized_pnl;

            // For futures with REALIZED_ONLY accounting: all PnL is realized due to daily
            // settlement
            if (pnl_accounting_.method == PnLAccountingMethod::REALIZED_ONLY) {
                daily_realized_pnl += mark_to_market_pnl;
                pos.unrealized_pnl = Decimal(0.0);  // Always zero for futures
                // Reset average price to current price after daily settlement
                // This prevents double-counting: next day's mark-to-market only captures that day's
                // change
                pos.average_price = current_price;
            } else {
                // For other accounting methods, use traditional unrealized PnL
                pos.unrealized_pnl = Decimal(mark_to_market_pnl);
            }

            // Store PnL based on mode:
            // - Backtest mode: store DAILY PnL only (for correct equity curve accumulation)
            // - Live mode: store CUMULATIVE PnL (for compatibility with existing systems)
            if (is_backtest_mode()) {
                // Backtest: store only today's daily PnL
                pos.realized_pnl = Decimal(daily_realized_pnl);
            } else {
                // Live: accumulate PnL across days
                pos.realized_pnl = Decimal(previous_realized_pnl + daily_realized_pnl);
            }

            // Add position-specific realized PnL to accounting system
            if (std::abs(daily_realized_pnl) > 1e-6) {
                pnl_accounting_.add_realized_pnl(daily_realized_pnl);
            }
            if (std::abs(mark_to_market_pnl) > 1e-6 &&
                pnl_accounting_.method != PnLAccountingMethod::REALIZED_ONLY) {
                pnl_accounting_.add_unrealized_pnl(mark_to_market_pnl);
            }

            INFO("Position update for " + symbol + ": prev_qty=" +
                 std::to_string(previous_quantity) + " new_qty=" + std::to_string(final_position) +
                 " prev_avg=" + std::to_string(previous_avg_price) +
                 " new_avg=" + std::to_string(static_cast<double>(pos.average_price)) +
                 " current_price=" + std::to_string(current_price) +
                 " daily_pnl=" + std::to_string(daily_realized_pnl) +
                 " stored_realized_pnl=" + std::to_string(static_cast<double>(pos.realized_pnl)) +
                 " backtest_mode=" + std::string(is_backtest_mode() ? "true" : "false"));

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

bool TrendFollowingStrategy::overlay_series(const std::string& symbol, OverlaySeries* out) const {
    const auto it = instrument_data_.find(symbol);
    if (it == instrument_data_.end() || out == nullptr) return false;
    const InstrumentData& data = it->second;
    if (data.price_history.empty()) return false;
    out->day = data.overlay_days;
    out->returns = data.overlay_returns;
    out->close = data.price_history.back();
    out->multiplier = data.contract_size;
    // The contract size is cached on the symbol's first sized bar (on_data). Before that the cache
    // holds its initial 1.0, so a symbol still in warm-up answers from the registry directly: the
    // weight of one contract is the instrument's, whether or not the sleeve signals it yet.
    if (data.contract_size == 1.0 && registry_) {
        const std::string lookup = symbol.substr(0, symbol.find(".v."));
        if (registry_->has_instrument(lookup)) {
            if (const auto instrument = registry_->get_instrument(lookup)) {
                out->multiplier = instrument->get_multiplier();
            }
        }
    }
    out->jump_sigma_daily = data.estimate.valid ? data.estimate.jump_sigma_daily : 0.0;
    out->optimal_position = data.optimal_position;
    out->forecast = data.current_forecast;
    out->signalling = is_signalling(symbol);
    out->slow_rule_zeroed = data.slow_rule_zeroed;
    out->opt_day = data.opt_days;
    out->opt_close = data.opt_closes;
    out->opt_level = data.opt_levels;
    return true;
}

bool TrendFollowingStrategy::is_signalling(const std::string& symbol) const {
    // on_data's warm-up test, read without a feed (T-OPT E-7): a symbol whose price history holds
    // fewer prices than the longest EMA window gets no forecast and no target of its own.
    int max_window = 0;
    for (const auto& window_pair : trend_config_.ema_windows) {
        max_window = std::max(max_window, window_pair.second);
    }
    auto it = instrument_data_.find(symbol);
    return it != instrument_data_.end() &&
           !(it->second.price_history.size() < static_cast<size_t>(max_window));
}

std::unordered_map<std::string, Position> TrendFollowingStrategy::get_target_positions() const {
    std::unordered_map<std::string, Position> target_positions;

    // Build positions from instrument_data_ which has the calculated final_position values
    for (const auto& [symbol, instrument_data] : instrument_data_) {
        Position pos;
        pos.symbol = symbol;
        pos.quantity = instrument_data.final_position;

        // For futures with daily mark-to-market, use current market price
        // This matches REALIZED_ONLY accounting behavior where average_price
        // resets to current_price after daily settlement (calculate_position line 552)
        if (!instrument_data.price_history.empty()) {
            pos.average_price = instrument_data.price_history.back();
        }

        // Copy PnL values from the positions_ map
        // Note: We deliberately do NOT copy average_price from positions_ map
        // because it may be stale if the position hasn't changed recently
        auto pos_it = positions_.find(symbol);
        if (pos_it != positions_.end()) {
            pos.realized_pnl = pos_it->second.realized_pnl;
            pos.unrealized_pnl = pos_it->second.unrealized_pnl;
            // Do NOT overwrite average_price - keep the current market price
        }

        pos.last_update = instrument_data.last_update;
        target_positions[symbol] = pos;
    }

    return target_positions;
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

std::unordered_map<std::string, double> TrendFollowingStrategy::get_weights() const {
    if (!weight_cache_.empty()) {
        return weight_cache_;
    }
    auto metadata_result = db_->get_contract_metadata();
    if (!metadata_result.is_ok()) {
        ERROR(std::string("Failed to get contract metadata: ")
                  .append(metadata_result.error()->what()));
        return {};
    }

    // Get actually-traded symbols from HLCV table to filter phantom instruments
    std::unordered_set<std::string> traded_base_symbols;
    auto hlcv_symbols_result = db_->get_symbols(AssetClass::FUTURES);
    if (hlcv_symbols_result.is_ok()) {
        for (const auto& sym : hlcv_symbols_result.value()) {
            // Strip .v.0 / .c.0 suffix to get base symbol
            auto dot_pos = sym.find('.');
            std::string base = (dot_pos != std::string::npos) ? sym.substr(0, dot_pos) : sym;
            // Exclude full-size ES (filtered in portfolio apps)
            if (base != "ES") {
                traded_base_symbols.insert(base);
            }
        }
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

    // Build map of sector -> list of symbols, filtered to only traded instruments
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
                // Only include symbols that are actually in the HLCV table
                if (traded_base_symbols.empty() || traded_base_symbols.count(symbol) > 0) {
                    sector_to_symbols[sector].push_back(symbol);
                }
            }
        }
    }

    // Compute weights
    std::unordered_map<std::string, double> symbol_weights;
    int total_sectors = static_cast<int>(sector_to_symbols.size());

    if (total_sectors == 0)
        return symbol_weights;

    double sector_weight = 1.0 / total_sectors;

    // Maximum weight any single symbol can have within its sector (50% of sector weight)
    const double MAX_SYMBOL_TO_SECTOR_RATIO = 0.50;

    // Symbols capped below their equal share; the closing normalization must not
    // re-inflate them.
    std::unordered_set<std::string> capped_symbols;

    for (const auto& [sector, symbols] : sector_to_symbols) {
        int num_symbols = static_cast<int>(symbols.size());
        if (num_symbols == 0)
            continue;

        double per_symbol_weight = sector_weight / num_symbols;

        // Cap individual symbol weight to maximum % of sector allocation
        double max_symbol_weight = sector_weight * MAX_SYMBOL_TO_SECTOR_RATIO;
        double capped_weight = std::min(per_symbol_weight, max_symbol_weight);

        for (const auto& symbol : symbols) {
            symbol_weights[symbol] = capped_weight;

            // Log when a symbol's weight is capped
            if (capped_weight < per_symbol_weight) {
                capped_symbols.insert(symbol);
                INFO("Symbol " + symbol + " in sector " + sector +
                     " weight capped from " + std::to_string(per_symbol_weight * 100.0) +
                     "% to " + std::to_string(capped_weight * 100.0) +
                     "% (max 50% of sector allocation)");
            }
        }
    }

    // Normalize weights to sum to 100%. Scale only the uncapped symbols over the
    // budget the caps freed; scaling everything re-inflates capped symbols past
    // MAX_SYMBOL_TO_SECTOR_RATIO of their sector allocation.
    double capped_sum = 0.0;
    double uncapped_sum = 0.0;
    for (const auto& [symbol, weight] : symbol_weights) {
        (capped_symbols.count(symbol) ? capped_sum : uncapped_sum) += weight;
    }
    const double weight_sum = capped_sum + uncapped_sum;
    if (weight_sum > 0.0 && std::abs(weight_sum - 1.0) > 0.001) {
        if (uncapped_sum > 0.0 && capped_sum < 1.0) {
            const double scale = (1.0 - capped_sum) / uncapped_sum;
            for (auto& [symbol, weight] : symbol_weights) {
                if (capped_symbols.count(symbol) == 0) {
                    weight *= scale;
                }
            }
        } else {
            // Every symbol capped (all sectors single-symbol): plain scaling is the
            // only way back to a fully-invested portfolio.
            for (auto& [symbol, weight] : symbol_weights) {
                weight /= weight_sum;
            }
        }
    }

    weight_cache_ = symbol_weights;
    return weight_cache_;
}

double TrendFollowingStrategy::calculate_position(const std::string& symbol, double forecast,
                                                  double price, double volatility,
                                                  double* optimal_position) const {
    if (optimal_position != nullptr) *optimal_position = 0.0;
    // Validation
    if (std::isnan(forecast) || std::isinf(forecast) || std::abs(forecast) > 20.0) {
        WARN("Invalid forecast in position calculation for " + symbol + ", using 0.0");
        return 0.0;
    }

    if (std::isnan(price) || price <= 0.0) {
        WARN("Invalid price in position calculation for " + symbol + ": " + std::to_string(price));
        // Try to find last valid price from instrument data
        auto inst_it = instrument_data_.find(symbol);
        if (inst_it != instrument_data_.end() && !inst_it->second.price_history.empty()) {
            price = inst_it->second.price_history.back();
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

        // The optimal position, before any limit
        if (optimal_position != nullptr) *optimal_position = position;

        // LOOP_SPEC section 3.2 (D4, D13, D14): the position is published as it is, unbuffered,
        // unrounded and unclamped. The per-name cap, the buffer and the rounding are the
        // portfolio's one pass's.
        return position;
    } else {
        ERROR("No instrument data found for " + symbol);
        return 0.0;  // Use neutral position
    }
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
        ERROR("CRITICAL: Instrument registry not initialized when requesting multiplier for " +
              symbol);
        throw std::runtime_error("Instrument registry not initialized for symbol: " + symbol);
    }

    if (!registry_->has_instrument(base_symbol)) {
        ERROR("CRITICAL: Instrument " + base_symbol +
              " not found in registry! Cannot continue without proper multiplier.");
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
            ERROR("CRITICAL: Invalid multiplier " + std::to_string(multiplier) + " for " +
                  base_symbol + ". Multiplier must be positive.");
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

std::unordered_map<int, double> TrendFollowingStrategy::get_ema_values(
    const std::string& symbol, const std::vector<int>& windows) const {
    std::unordered_map<int, double> ema_values;

    // Get instrument data for this symbol
    auto it = instrument_data_.find(symbol);
    if (it == instrument_data_.end() || it->second.price_history.empty()) {
        // Return empty map if no data available
        return ema_values;
    }

    const auto& price_deque = it->second.price_history;
    std::vector<double> price_history(price_deque.begin(), price_deque.end());
    // T-ROLLX: the EMAs read the adjusted level (LOOP_SPEC v6.1 section 2.3), as the forecast's do.
    const auto& id_deque = it->second.bar_instrument_ids;
    const std::vector<std::string> instrument_ids(id_deque.begin(), id_deque.end());
    const roll_series::Series series = roll_series::build_series(price_history, instrument_ids);

    // Calculate EMA for each requested window
    for (int window : windows) {
        auto ema_series = calculate_ewma(series.adjusted, window);
        if (!ema_series.empty()) {
            // Return the most recent EMA value
            ema_values[window] = ema_series.back();
        }
    }

    return ema_values;
}

int TrendFollowingStrategy::get_max_required_lookback() const {
    int max_lookback = 0;

    // Find the maximum EMA window (use the second value in each pair, which is the longer window)
    for (const auto& [short_window, long_window] : trend_config_.ema_windows) {
        max_lookback = std::max(max_lookback, long_window);
    }

    // Also consider volatility lookback (though vol_lookback_long is typically for historical
    // averaging, not for indicator validity, but we include it for completeness)
    max_lookback = std::max(max_lookback, trend_config_.vol_lookback_short);
    // Note: vol_lookback_long (typically 2520) is for long-term averaging and doesn't affect
    // when indicators become valid, so we don't include it in warmup calculation

    return max_lookback;
}

}  // namespace trade_ngin