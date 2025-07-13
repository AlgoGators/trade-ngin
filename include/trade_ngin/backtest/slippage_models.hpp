#pragma once

#include <memory>
#include <unordered_map>
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"

namespace trade_ngin {
namespace backtest {

/**
 * @brief Configuration for volume-based slippage model
 */
struct VolumeSlippageConfig {
    double price_impact_coefficient{1e-6};  // Price impact per unit of volume
    double min_volume_ratio{0.01};          // Minimum volume ratio for slippage calc
    double max_volume_ratio{0.1};           // Maximum volume ratio before extra impact
    double volatility_multiplier{1.5};      // Increase slippage in volatile periods
};

/**
 * @brief Configuration for spread-based slippage
 */
struct SpreadSlippageConfig {
    double min_spread_bps{1.0};            // Minimum spread in basis points
    double spread_multiplier{1.2};         // Multiply spread for urgency
    double market_impact_multiplier{1.5};  // Additional impact for market orders
};

/**
 * @brief Interface for slippage models
 */
class SlippageModel {
public:
    virtual ~SlippageModel() = default;

    /**
     * @brief Calculate price with slippage
     * @param price Original price
     * @param quantity Trade quantity
     * @param side Trade side
     * @param market_data Optional market data for context
     * @return Adjusted price with slippage
     */
    virtual double calculate_slippage(
        double price, double quantity, Side side,
        const std::optional<Bar>& market_data = std::nullopt) const = 0;

    /**
     * @brief Calculate price with slippage (Decimal overload)
     * @param price Original price
     * @param quantity Trade quantity
     * @param side Trade side
     * @param market_data Optional market data for context
     * @return Adjusted price with slippage
     */
    virtual Decimal calculate_slippage(const Decimal& price, const Decimal& quantity, Side side,
                                       const std::optional<Bar>& market_data = std::nullopt) const {
        double result = calculate_slippage(static_cast<double>(price),
                                           static_cast<double>(quantity), side, market_data);
        return Decimal(result);
    }

    /**
     * @brief Update model parameters based on market data
     * @param market_data New market data
     */
    virtual void update(const Bar& market_data) = 0;
};

/**
 * @brief Volume-based slippage implementation
 */
class VolumeSlippageModel : public SlippageModel {
public:
    explicit VolumeSlippageModel(VolumeSlippageConfig config);

    double calculate_slippage(double price, double quantity, Side side,
                              const std::optional<Bar>& market_data = std::nullopt) const override;

    void update(const Bar& market_data) override;

private:
    VolumeSlippageConfig config_;
    std::unordered_map<std::string, double> average_volumes_;
    std::unordered_map<std::string, double> volatilities_;
};

/**
 * @brief Spread and market impact based slippage
 */
class SpreadSlippageModel : public SlippageModel {
public:
    explicit SpreadSlippageModel(SpreadSlippageConfig config);

    double calculate_slippage(double price, double quantity, Side side,
                              const std::optional<Bar>& market_data = std::nullopt) const override;

    void update(const Bar& market_data) override;

private:
    SpreadSlippageConfig config_;
    std::unordered_map<std::string, double> spread_estimates_;
};

/**
 * @brief Factory for creating slippage models
 */
class SlippageModelFactory {
public:
    /**
     * @brief Create volume-based slippage model
     */
    static std::unique_ptr<SlippageModel> create_volume_model(const VolumeSlippageConfig& config);

    /**
     * @brief Create spread-based slippage model
     */
    static std::unique_ptr<SlippageModel> create_spread_model(const SpreadSlippageConfig& config);
};

}  // namespace backtest
}  // namespace trade_ngin