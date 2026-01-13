// include/trade_ngin/strategy/mean_reversion.hpp
#pragma once

#include <memory>
#include <utility>
#include <vector>
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"

namespace trade_ngin {

/**
 * @brief Configuration specific to mean reversion strategy
 */
struct MeanReversionConfig {
    int lookback_period{20};          // Lookback period for moving average
    double entry_threshold{2.0};      // Z-score threshold for entry
    double exit_threshold{0.5};       // Z-score threshold for exit
    double risk_target{0.15};         // Target annualized risk level (lower than trend following)
    double position_size{0.1};        // Maximum position size as fraction of capital
    int vol_lookback{20};             // Lookback for volatility calculation
    bool use_stop_loss{true};         // Whether to use stop loss
    double stop_loss_pct{0.05};       // Stop loss percentage (5%)
};

/**
 * @brief Data structure for storing instrument mean reversion data
 */
struct MeanReversionInstrumentData {
    // Price data
    std::vector<double> price_history;
    double current_price = 0.0;

    // Mean reversion indicators
    double moving_average = 0.0;
    double std_deviation = 0.0;
    double z_score = 0.0;

    // Position data
    double target_position = 0.0;
    double entry_price = 0.0;

    // Volatility
    double current_volatility = 0.01;
    std::vector<double> volatility_history;

    // Timestamp of last update
    Timestamp last_update;
};

/**
 * @brief Simple mean reversion strategy using z-score
 *
 * Strategy Logic:
 * - Calculate moving average and standard deviation over lookback period
 * - Enter short when z-score > entry_threshold (price too high)
 * - Enter long when z-score < -entry_threshold (price too low)
 * - Exit when z-score crosses back towards 0 past exit_threshold
 */
class MeanReversionStrategy : public BaseStrategy {
public:
    /**
     * @brief Constructor
     * @param id Strategy identifier
     * @param config Base strategy configuration
     * @param mr_config Mean reversion specific configuration
     * @param db Database interface
     * @param registry Instrument registry for accessing instrument data
     */
    MeanReversionStrategy(std::string id, StrategyConfig config, MeanReversionConfig mr_config,
                          std::shared_ptr<PostgresDatabase> db,
                          std::shared_ptr<InstrumentRegistry> registry = nullptr);

    /**
     * @brief Process new market data
     * @param data Vector of price bars
     * @return Result indicating success or failure
     */
    Result<void> on_data(const std::vector<Bar>& data) override;

    /**
     * @brief Initialize strategy
     * @return Result indicating success or failure
     */
    Result<void> initialize() override;

    /**
     * @brief Get price history for all symbols
     * @return Map of symbol to price history
     */
    std::unordered_map<std::string, std::vector<double>> get_price_history() const override {
        std::unordered_map<std::string, std::vector<double>> history;
        for (const auto& [symbol, data] : instrument_data_) {
            history[symbol] = data.price_history;
        }
        return history;
    }

    /**
     * @brief Get current z-score for a symbol
     * @param symbol Instrument symbol
     * @return Current z-score value
     */
    double get_z_score(const std::string& symbol) const {
        auto it = instrument_data_.find(symbol);
        if (it != instrument_data_.end()) {
            return it->second.z_score;
        }
        return 0.0;
    }

    /**
     * @brief Get current position for a symbol
     * @param symbol Instrument symbol
     * @return Current position value
     */
    double get_position(const std::string& symbol) const {
        auto it = instrument_data_.find(symbol);
        if (it != instrument_data_.end()) {
            return it->second.target_position;
        }
        return 0.0;
    }

    /**
     * @brief Get instrument data for a symbol
     * @param symbol Instrument symbol
     * @return Pointer to instrument data or nullptr
     */
    const MeanReversionInstrumentData* get_instrument_data(const std::string& symbol) const {
        auto it = instrument_data_.find(symbol);
        if (it != instrument_data_.end()) {
            return &it->second;
        }
        return nullptr;
    }

protected:
    /**
     * @brief Validate strategy configuration
     * @return Result indicating if config is valid
     */
    Result<void> validate_config() const override;

private:
    MeanReversionConfig mr_config_;
    std::shared_ptr<InstrumentRegistry> registry_;
    std::unordered_map<std::string, MeanReversionInstrumentData> instrument_data_;

    /**
     * @brief Calculate simple moving average
     * @param prices Price series
     * @param period Lookback period
     * @return Moving average value
     */
    double calculate_sma(const std::vector<double>& prices, int period) const;

    /**
     * @brief Calculate standard deviation
     * @param prices Price series
     * @param period Lookback period
     * @param mean Mean value
     * @return Standard deviation
     */
    double calculate_std_dev(const std::vector<double>& prices, int period, double mean) const;

    /**
     * @brief Calculate z-score
     * @param price Current price
     * @param mean Moving average
     * @param std_dev Standard deviation
     * @return Z-score
     */
    double calculate_z_score(double price, double mean, double std_dev) const;

    /**
     * @brief Calculate position size based on volatility
     * @param symbol Instrument symbol
     * @param price Current price
     * @param volatility Current volatility
     * @return Position size
     */
    double calculate_position_size(const std::string& symbol, double price, double volatility) const;

    /**
     * @brief Calculate volatility using standard deviation of returns
     * @param prices Price history
     * @param lookback Lookback period
     * @return Annualized volatility
     */
    double calculate_volatility(const std::vector<double>& prices, int lookback) const;

    /**
     * @brief Generate trading signal based on z-score
     * @param symbol Instrument symbol
     * @param data Instrument data
     * @return Trading signal (-1 = short, 0 = flat, 1 = long)
     */
    double generate_signal(const std::string& symbol, const MeanReversionInstrumentData& data) const;
};

}  // namespace trade_ngin
