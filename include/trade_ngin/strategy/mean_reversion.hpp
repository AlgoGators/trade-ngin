// include/trade_ngin/strategy/mean_reversion.hpp
#pragma once

#include <deque>
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
    bool allow_fractional_shares{true};   // Allow fractional share quantities
    double fractional_min_price{1.0};     // Min price for fractional eligibility ($1.00 industry standard)
    double fractional_min_adv{50000.0};   // Min ADV (shares/day) for fractional eligibility
};

/**
 * @brief Data structure for storing instrument mean reversion data
 */
struct MeanReversionInstrumentData {
    // Price data
    std::deque<double> price_history;
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
    std::deque<double> volatility_history;

    // Volume tracking for ADV-based fractional share eligibility
    double avg_daily_volume = 0.0;
    size_t volume_sample_count = 0;

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
    MeanReversionStrategy(std::string id, StrategyConfig config, MeanReversionConfig mr_config,
                          std::shared_ptr<PostgresDatabase> db,
                          std::shared_ptr<InstrumentRegistry> registry = nullptr);

    Result<void> on_data(const std::vector<Bar>& data) override;
    Result<void> initialize() override;

    std::unordered_map<std::string, std::vector<double>> get_price_history() const override {
        std::unordered_map<std::string, std::vector<double>> history;
        for (const auto& [symbol, data] : instrument_data_) {
            history[symbol] = std::vector<double>(data.price_history.begin(), data.price_history.end());
        }
        return history;
    }

    double get_z_score(const std::string& symbol) const {
        auto it = instrument_data_.find(symbol);
        if (it != instrument_data_.end()) {
            return it->second.z_score;
        }
        return 0.0;
    }

    double get_position(const std::string& symbol) const {
        auto it = instrument_data_.find(symbol);
        if (it != instrument_data_.end()) {
            return it->second.target_position;
        }
        return 0.0;
    }

    const MeanReversionInstrumentData* get_instrument_data(const std::string& symbol) const {
        auto it = instrument_data_.find(symbol);
        if (it != instrument_data_.end()) {
            return &it->second;
        }
        return nullptr;
    }

protected:
    Result<void> validate_config() const override;

private:
    MeanReversionConfig mr_config_;
    std::shared_ptr<InstrumentRegistry> registry_;
    std::unordered_map<std::string, MeanReversionInstrumentData> instrument_data_;

    double calculate_sma(const std::deque<double>& prices, int period) const;
    double calculate_std_dev(const std::deque<double>& prices, int period, double mean) const;
    double calculate_z_score(double price, double mean, double std_dev) const;
    double calculate_position_size(const std::string& symbol, double price, double volatility) const;
    double calculate_volatility(const std::deque<double>& prices, int lookback) const;
    double generate_signal(const std::string& symbol, const MeanReversionInstrumentData& data) const;

    /**
     * @brief Trim price/volatility history to prevent unbounded memory growth
     */
    void trim_history(MeanReversionInstrumentData& data) const;
};

}  // namespace trade_ngin
