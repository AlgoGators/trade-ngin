#pragma once

#include "trade_ngin/backtest/backtest_engine.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#include "trade_ngin/core/config_base.hpp"
#include "trade_ngin/core/error.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <memory>
#include <filesystem>

namespace trade_ngin {
namespace backtest {

/**
 * @brief Comprehensive backtest configuration integrating all required configs
 * This class combines all configurations needed for a trend following backtest
 * and provides methods to save to and load from a single config file.
 */
class BacktestConfigManager : public ConfigBase {
public:
    /**
     * @brief Constructor with default paths
     * @param config_dir Directory to store config files
     */
    explicit BacktestConfigManager(
        const std::filesystem::path& config_dir = "configs/backtest");

    /**
     * @brief Create a default configuration
     * @return Result containing the default configuration
     */
    static Result<BacktestConfigManager> create_default();

    /**
     * @brief Save configuration to the specified file
     * @param filename Filename (without path)
     * @return Result indicating success or failure
     */
    Result<void> save(const std::string& filename = "backtest_config.json") const;

    /**
     * @brief Load configuration from the specified file
     * @param filename Filename (without path)
     * @return Result indicating success or failure
     */
    Result<void> load(const std::string& filename = "backtest_config.json");

    /**
     * @brief Apply configuration to a backtest engine
     * @param engine Backtest engine to configure
     * @return Result indicating success or failure
     */
    Result<void> apply_to_engine(BacktestEngine& engine) const;

    // JSON serialization methods
    nlohmann::json to_json() const override;
    void from_json(const nlohmann::json& j) override;

    // Configuration getters
    const BacktestConfig& get_backtest_config() const { return backtest_config_; }
    const StrategyConfig& get_strategy_config() const { return strategy_config_; }
    const TrendFollowingConfig& get_trend_config() const { return trend_config_; }

    // Configuration setters
    void set_backtest_config(const BacktestConfig& config) { backtest_config_ = config; }
    void set_strategy_config(const StrategyConfig& config) { strategy_config_ = config; }
    void set_trend_config(const TrendFollowingConfig& config) { trend_config_ = config; }

private:
    BacktestConfig backtest_config_;
    StrategyConfig strategy_config_;
    TrendFollowingConfig trend_config_;
    std::filesystem::path config_dir_;
};

} // namespace backtest
} // namespace trade_ngin 