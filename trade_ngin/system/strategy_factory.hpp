#pragma once
#include "strategy.hpp"

class StrategyFactory {
public:
    enum class StrategyType {
        TREND_FOLLOWING,
        MEAN_REVERSION,
        STATISTICAL_ARBITRAGE,
        MARKET_MAKING,
        ML_BASED,
        CUSTOM
    };

    static std::unique_ptr<Strategy> createStrategy(
        StrategyType type,
        const std::string& config_path
    );

    template<typename T>
    static void registerStrategy(StrategyType type) {
        registry_[type] = []() { return std::make_unique<T>(); };
    }

private:
    static std::unordered_map<
        StrategyType,
        std::function<std::unique_ptr<Strategy>()>
    > registry_;
}; 