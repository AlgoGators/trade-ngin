// include/trade_ngin/portfolio/portfolio_manager.hpp
#pragma once

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/state_manager.hpp"
#include "trade_ngin/data/market_data_bus.hpp"
#include "trade_ngin/strategy/strategy_interface.hpp"
#include "trade_ngin/optimization/dynamic_optimizer.hpp"
#include "trade_ngin/risk/risk_manager.hpp"
#include <memory>
#include <unordered_map>
#include <mutex>
#include <numeric>
#include <iostream>

namespace trade_ngin {

/**
 * @brief Configuration for portfolio management
 */
struct PortfolioConfig {
    double total_capital{0.0};            // Total portfolio capital
    double reserve_capital{0.0};          // Capital to keep in reserve
    double max_strategy_allocation{1.0};   // Maximum allocation to any strategy
    double min_strategy_allocation{0.0};   // Minimum allocation to any strategy
    bool use_optimization{false};         // Whether to use position optimization
    bool use_risk_management{false};      // Whether to use risk management
    DynamicOptConfig opt_config;          // Optimization configuration
    RiskConfig risk_config;               // Risk management configuration
};

/**
 * @brief Manages multiple strategies and their allocations
 * Optionally applies optimization and risk management
 */
class PortfolioManager {
public:
    /**
     * @brief Constructor
     * @param config Portfolio configuration
     * @param id Optional identifier for this manager
     */
    explicit PortfolioManager(PortfolioConfig config, std::string id = "PORTFOLIO_MANAGER")
        : config_(std::move(config)),
        id_(std::move(id)) {

        // Initialize optimizer if enabled
        if (config_.use_optimization) {
            optimizer_ = std::make_unique<DynamicOptimizer>(config_.opt_config);
        }

        // Initialize risk manager if enabled
        if (config_.use_risk_management) {
            try {
                risk_manager_ = std::make_unique<RiskManager>(config_.risk_config);
                if (!risk_manager_) {
                    throw std::runtime_error("Failed to create risk manager");
                }
            } catch (const std::exception& e) {
                std::cerr << "Error initializing risk manager: " << e.what() << std::endl;
                throw;
            }
        }
        
        // Initialize with the provided ID
        ComponentInfo info{
            ComponentType::PORTFOLIO_MANAGER,
            ComponentState::INITIALIZED,
            id_,  // Use the provided ID
            "",
            std::chrono::system_clock::now(),
            {
                {"total_capital", config_.total_capital},
                {"reserve_capital", config_.reserve_capital}
            }
        };

        auto register_result = StateManager::instance().register_component(info);
        if (register_result.is_error()) {
            throw std::runtime_error(register_result.error()->what());
        }

        // Subscribe to market data and position updates
        MarketDataCallback callback = [this](const MarketDataEvent& event) {
            if (event.type == MarketDataEventType::POSITION_UPDATE) {
                // Handle position updates
                std::string strategy_id = event.string_fields.at("strategy_id");
                auto it = strategies_.find(strategy_id);
                if (it != strategies_.end()) {
                    Position pos;
                    pos.symbol = event.symbol;
                    pos.quantity = event.numeric_fields.at("quantity");
                    pos.average_price = event.numeric_fields.at("price");
                    pos.last_update = event.timestamp;
                    it->second.current_positions[event.symbol] = pos;
                }
            }
            else if (event.type == MarketDataEventType::BAR) {
                // Convert to Bar and process
                Bar bar;
                bar.timestamp = event.timestamp;
                bar.symbol = event.symbol;
                bar.open = event.numeric_fields.at("open");
                bar.high = event.numeric_fields.at("high");
                bar.low = event.numeric_fields.at("low");
                bar.close = event.numeric_fields.at("close");
                bar.volume = event.numeric_fields.at("volume");

                std::vector<Bar> bars{bar};
                auto result = this->process_market_data(bars);
                if (result.is_error()) {
                    ERROR("Error processing market data: " + std::string(result.error()->what()));
                }
            }
        };

        SubscriberInfo sub_info{
            "PORTFOLIO_MANAGER",
            {MarketDataEventType::BAR, MarketDataEventType::POSITION_UPDATE},
            {},  // Subscribe to all symbols
            callback
        };

        auto subscribe_result = MarketDataBus::instance().subscribe(sub_info);
        if (subscribe_result.is_error()) {
            throw std::runtime_error(subscribe_result.error()->what());
        }

        StateManager::instance().update_state("PORTFOLIO_MANAGER", ComponentState::RUNNING);
    }

    /**
     * @brief Add a strategy to the portfolio
     * @param strategy Strategy to add
     * @param initial_allocation Initial capital allocation
     * @param use_optimization Whether this strategy uses optimization
     * @param use_risk_management Whether this strategy uses risk management
     * @return Result indicating success or failure
     */
    Result<void> add_strategy(
        std::shared_ptr<StrategyInterface> strategy,
        double initial_allocation,
        bool use_optimization = false,
        bool use_risk_management = false
    );

    /**
     * @brief Process new market data
     * @param data New market data
     * @return Result indicating success or failure
     */
    Result<void> process_market_data(const std::vector<Bar>& data);

    /**
     * @brief Update strategy allocations
     * @param allocations Map of strategy ID to allocation
     * @return Result indicating success or failure
     */
    Result<void> update_allocations(
        const std::unordered_map<std::string, double>& allocations
    );

    /**
     * @brief Get current portfolio positions
     * @return Map of symbol to aggregated position
     */
    std::unordered_map<std::string, Position> get_portfolio_positions() const;

    /**
     * @brief Get position changes needed
     * @return Map of symbol to required position change
     */
    std::unordered_map<std::string, double> get_required_changes() const;

private:
    PortfolioConfig config_;
    std::string id_;
    std::unique_ptr<DynamicOptimizer> optimizer_;
    std::unique_ptr<RiskManager> risk_manager_;

    struct StrategyInfo {
        std::shared_ptr<StrategyInterface> strategy;
        double allocation;
        bool use_optimization;
        bool use_risk_management;
        std::unordered_map<std::string, Position> current_positions;
        std::unordered_map<std::string, Position> target_positions;
    };

    std::unordered_map<std::string, StrategyInfo> strategies_;
    mutable std::mutex mutex_;
    const std::string instance_id_;

    /**
     * @brief Optimize positions for strategies that use optimization
     * @return Result indicating success or failure
     */
    Result<void> optimize_positions();

    /**
     * @brief Apply risk management to positions
     * @return Result indicating success or failure
     */
    Result<void> apply_risk_management();

    /**
     * @brief Validate allocations sum to 1
     * @param allocations Strategy allocations
     * @return Result indicating if allocations are valid
     */
    Result<void> validate_allocations(
        const std::unordered_map<std::string, double>& allocations
    ) const;
};

} // namespace trade_ngin