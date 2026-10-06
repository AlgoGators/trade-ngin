// include/trade_ngin/strategy/trend_following.hpp
#pragma once

#include <deque>
#include <memory>
#include <utility>
#include <vector>
#include "trade_ngin/data/roll_series.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"
#include "trade_ngin/strategy/trend_estimator.hpp"
#include "trade_ngin/strategy/vol_annualisation.hpp"

namespace trade_ngin {

/**
 * @brief Configuration specific to trend following strategy
 */
struct TrendFollowingConfig {
    double risk_target{0.2};            // Target annualized risk level
    double fx_rate{1.0};                // FX conversion rate
    double idm{2.5};                    // Instrument diversification multiplier
    std::vector<std::pair<int, int>> ema_windows{
        // EMA window pairs for crossovers
        {2, 8}, {4, 16}, {8, 32}, {16, 64}, {32, 128}, {64, 256}};
    int vol_lookback_short{32};   // Short lookback for volatility calculation
    int vol_lookback_long{2520};  // Not read by the estimator: its long-run mean is 2,520 values
    size_t max_history_size{0};   // Set to the estimators' window (trend_estimator::kWindowBars)
    std::vector<std::pair<int, double>> fdm{{1, 1.0},  {2, 1.03}, {3, 1.08},
                                            {4, 1.13}, {5, 1.19}, {6, 1.26}};
    // The equity slow rule (LOOP_SPEC section 2.5, D40): for these symbols (base names, "MES" for
    // "MES.v.0") a negative combined forecast stands only when every one of these pairs' scaled
    // forecasts is negative, and is 0 otherwise. Empty symbols: the sleeve is not ruled. The runner
    // sets both on the book's first sleeve from portfolio.json's equity_slow_rule; every pair
    // named must be one of the sleeve's ema_windows.
    std::vector<std::string> equity_slow_symbols;
    std::vector<std::pair<int, int>> equity_slow_pairs;
};

/**
 * @brief The FAST sleeve's configuration: TrendFollowingStrategy on the four fast EMA pairs, a 16-bar
 * short vol span and a 0.25 risk target. The FAST sleeve is this configuration
 * of the one trend class, not a class of its own.
 */
inline TrendFollowingConfig fast_trend_following_config() {
    TrendFollowingConfig config;
    config.risk_target = 0.25;
    config.ema_windows = {{2, 8}, {4, 16}, {8, 32}, {16, 64}};
    config.vol_lookback_short = 16;
    return config;
}

/**
 * @brief Data structure for storing instrument data
 */
struct InstrumentData {
    // Static instrument properties (cached from registry)
    double contract_size = 1.0;
    double weight = 1.0;

    // Dynamic forecast data (scalars only, vectors computed locally)
    double current_raw_forecast = 0.0;
    double current_scaled_forecast = 0.0;
    double current_forecast = 0.0;

    // Position data
    double raw_position = 0.0;
    double final_position = 0.0;

    // Market data (deque for O(1) front removal)
    std::deque<double> price_history;
    std::deque<Timestamp> bar_timestamps;  // each price_history bar's date (vol annualisation)
    std::deque<std::string> bar_instrument_ids;  // each bar's vendor contract id (T-ROLLX, roll_series.hpp)
    std::deque<double> volatility_history;
    double current_volatility = 0.01;

    // The history seeded before the first fed bar (seed_history): a bulk feed, which replaces the
    // fed history, keeps these bars in front of it.
    std::deque<double> seeded_prices;
    std::deque<Timestamp> seeded_timestamps;
    std::deque<std::string> seeded_instrument_ids;

    // The estimators at the last signal bar (trend_estimator.hpp), and the position they size
    // before any limit: (forecast / 10) x capital x IDM x weight x tau / (multiplier x price x
    // FX x sigma).
    trend_estimator::Estimate estimate;
    // The symbol's last bars that have a return, for the risk overlay's gate window (section 4):
    // each bar's date as a whole day number and its adjusted percentage return, oldest first. The
    // window is 252 dates on which any participant has a return, so 300 own bars cover it.
    std::vector<double> overlay_days;
    std::vector<double> overlay_returns;
    // The symbol's last 756 consumed bars for the optimiser's covariance (section 5.1): the date,
    // the raw close and the adjusted level of each.
    std::vector<double> opt_days;
    std::vector<double> opt_closes;
    std::vector<double> opt_levels;
    double optimal_position = 0.0;
    bool slow_rule_zeroed = false;  // the equity slow rule set the last forecast to 0

    // Timestamp of last update
    Timestamp last_update;
};

/**
 * @brief Multi-timeframe trend following strategy using EMA crossovers
 */
class TrendFollowingStrategy : public BaseStrategy {
public:
    /**
     * @brief Constructor
     * @param id Strategy identifier
     * @param config Base strategy configuration
     * @param trend_config Trend following specific configuration
     * @param db Database interface
     * @param registry Instrument registry for accessing instrument data
     */
    TrendFollowingStrategy(std::string id, StrategyConfig config, TrendFollowingConfig trend_config,
                           std::shared_ptr<PostgresDatabase> db,
                           std::shared_ptr<InstrumentRegistry> registry = nullptr);

    /**
     * @brief Process new market data
     * @param data Vector of price bars
     * @return Result indicating success or failure
     */
    Result<void> on_data(const std::vector<Bar>& data) override;

    /**
     * @brief Seed the estimators' history with the consumed bars that precede the first fed bar
     *        (the window's W bars reach back before a run's own window). Publishes and sizes nothing.
     * @param bars consumed bars of any number of symbols, each symbol's dated before its first fed bar
     */
    Result<void> seed_history(const std::vector<Bar>& bars) override;

    /**
     * @brief Initialize strategy
     * @return Result indicating success or failure
     */
    Result<void> initialize() override;

    /**
     * @brief Handle execution reports
     * @note Override to prevent base class from corrupting PnL data.
     *       TrendFollowingStrategy calculates PnL in on_data() with proper point_value multiplier.
     * @param report Execution report
     * @return Result indicating success or failure
     */
    Result<void> on_execution(const ExecutionReport& report) override;

    /**
     * @brief Return price history for a symbol
     * @param symbol Instrument symbol
     */
    std::unordered_map<std::string, std::vector<double>> get_price_history() const override {
        std::unordered_map<std::string, std::vector<double>> history;
        for (const auto& [symbol, data] : instrument_data_) {
            // Convert deque → vector for return
            history[symbol].assign(data.price_history.begin(), data.price_history.end());
        }
        return history;
    }

    /**
     * @brief Return current forecast for a symbol
     * @param symbol Instrument symbol
     * @return Current forecast value
     */
    double get_forecast(const std::string& symbol) const {
        auto it = instrument_data_.find(symbol);
        if (it != instrument_data_.end()) {
            return it->second.current_forecast;
        }
        return 0.0;  // Default value if not found
    }

    /**
     * @brief Return current position for a symbol
     * @param symbol Instrument symbol
     * @return Current position value
     */
    double get_position(const std::string& symbol) const {
        auto it = instrument_data_.find(symbol);
        if (it != instrument_data_.end()) {
            return it->second.final_position;
        }
        return 0.0;  // Default value if not found
    }

    /**
     * @brief Get a copy of the instrument data for a symbol
     * @param symbol Instrument symbol
     * @return Copy of the instrument data
     */
    const InstrumentData* get_instrument_data(const std::string& symbol) const {
        auto it = instrument_data_.find(symbol);
        if (it != instrument_data_.end()) {
            return &it->second;
        }
        return nullptr;  // Default value if not found
    }

    /**
     * @brief Get all instrument data
     * @return Map of instrument data by symbol
     */
    const std::unordered_map<std::string, InstrumentData>& get_all_instrument_data() const {
        return instrument_data_;
    }

    /**
     * @brief Get target positions from instrument data
     * @note Overrides base class to return positions calculated from instrument_data_
     *       which contains the properly computed final_position values
     * @return Map of positions by symbol with correct quantities and PnL
     */
    std::unordered_map<std::string, Position> get_target_positions() const override;

    /**
     * @brief false while `symbol`'s price history is shorter than the longest EMA window (on_data
     *        skips it: no forecast and no target of its own); true from then on (T-OPT E-7)
     */
    bool is_signalling(const std::string& symbol) const override;

    bool overlay_series(const std::string& symbol, OverlaySeries* out) const override;

    /**
     * @brief Get the correct point value multiplier for a futures symbol
     * @note Made public for use by live_trend.cpp to calculate PnL consistently
     */
    double get_point_value_multiplier(const std::string& symbol) const;

    /**
     * @brief Get EMA values for a symbol at specific windows
     * @param symbol Instrument symbol
     * @param windows Vector of EMA window sizes to calculate
     * @return Map of window sizes to EMA values
     */
    std::unordered_map<int, double> get_ema_values(const std::string& symbol, const std::vector<int>& windows) const;

    /**
     * @brief Get the maximum required lookback period for this strategy
     * @return Maximum lookback in days (max of EMA windows, volatility lookback, etc.)
     */
    int get_max_required_lookback() const;

    /**
     * @brief Get sector-budgeted symbol weights for position sizing
     * @return Map of symbol to weight (sums to 1.0; per-symbol capped at 50% of
     *         its sector allocation). Public so tests can pin the cap invariant.
     */
    std::unordered_map<std::string, double> get_weights() const;

protected:
    /**
     * @brief Validate strategy configuration
     * @return Result indicating if config is valid
     */
    Result<void> validate_config() const override;

private:
    TrendFollowingConfig trend_config_;

    std::shared_ptr<InstrumentRegistry> registry_;

    std::unordered_map<std::string, double> contract_size_cache_;
    mutable std::unordered_map<std::string, double> weight_cache_;

    std::unordered_map<std::string, InstrumentData> instrument_data_;

    // Previous day positions for PnL calculation

    /**
     * @brief Calculate EWMA for a price series
     * @param prices Price series
     * @param window EWMA window
     * @return Vector of EWMA values
     */
    std::vector<double> calculate_ewma(const std::vector<double>& prices, int window) const;

    /**
     * @brief Calculate position for a symbol
     * @param symbol Instrument symbol
     * @param forecast Trading forecast
     * @param weight Weight
     * @param price Current price
     * @param volatility Current volatility
     * @param optimal_position when given, receives the position before any limit
     * @return Target position
     */
    double calculate_position(const std::string& symbol, double forecast, double price,
                              double volatility, double* optimal_position = nullptr) const;
};

}  // namespace trade_ngin