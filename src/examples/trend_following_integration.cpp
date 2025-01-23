//src/examples/trend_following_integration.cpp
#include "trade_ngin/strategy/trend_following.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/optimization/dynamic_optimizer.hpp"
#include "trade_ngin/risk/risk_manager.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include <memory>
#include <string>

int main() {
    try {
        // Initialize database connection
        auto db = std::make_shared<PostgresDatabase>(
            "postgresql://user:password@localhost:5432/tradingdb"
        );
        auto connect_result = db->connect();
        if (connect_result.is_error()) {
            throw std::runtime_error(connect_result.error()->what());
        }

        // Configure trend following strategy
        StrategyConfig strategy_config;
        strategy_config.capital_allocation = 10000000.0;  // $10M allocation
        strategy_config.max_leverage = 4.0;
        strategy_config.asset_classes = {AssetClass::FUTURES};
        strategy_config.frequencies = {DataFrequency::DAILY};
        strategy_config.save_signals = true;
        strategy_config.save_positions = true;
        strategy_config.save_executions = true;

        TrendFollowingConfig trend_config;
        trend_config.risk_target = 0.2;
        trend_config.idm = 2.5;
        trend_config.fx_rate = 1.0;
        trend_config.use_position_buffering = true;
        trend_config.ema_windows = {
            {2, 8}, {4, 16}, {8, 32}, {16, 64}, {32, 128}, {64, 256}
        };

        // Create trend following strategy
        auto strategy = std::make_shared<TrendFollowingStrategy>(
            "TREND_1",
            strategy_config,
            trend_config,
            db
        );

        // Initialize strategy
        auto init_result = strategy->initialize();
        if (init_result.is_error()) {
            throw std::runtime_error(init_result.error()->what());
        }

        // Configure portfolio manager
        PortfolioConfig portfolio_config;
        portfolio_config.total_capital = 10000000.0;
        portfolio_config.reserve_capital = 1000000.0;
        portfolio_config.max_strategy_allocation = 1.0;
        portfolio_config.min_strategy_allocation = 0.0;
        portfolio_config.use_optimization = true;
        portfolio_config.use_risk_management = true;

        // Configure optimization
        DynamicOptConfig opt_config;
        opt_config.tau = 1.0;
        opt_config.capital = portfolio_config.total_capital;
        opt_config.asymmetric_risk_buffer = 0.1;
        opt_config.cost_penalty_scalar = 10;
        portfolio_config.opt_config = opt_config;

        // Configure risk management
        RiskConfig risk_config;
        risk_config.portfolio_var_limit = 0.15;
        risk_config.max_drawdown = 0.20;
        risk_config.jump_risk_threshold = 0.10;
        risk_config.max_correlation = 0.7;
        risk_config.max_gross_leverage = 4.0;
        risk_config.max_net_leverage = 2.0;
        risk_config.capital = portfolio_config.total_capital;
        portfolio_config.risk_config = risk_config;

        // Create portfolio manager
        PortfolioManager portfolio_manager(portfolio_config);

        // Add strategy to portfolio manager
        auto add_result = portfolio_manager.add_strategy(
            strategy,
            1.0,  // 100% allocation
            true, // Use optimization
            true  // Use risk management
        );
        if (add_result.is_error()) {
            throw std::runtime_error(add_result.error()->what());
        }

        // Start the strategy
        auto start_result = strategy->start();
        if (start_result.is_error()) {
            throw std::runtime_error(start_result.error()->what());
        }

        // Get market data for processing
        auto data_result = db->get_market_data(
            {"ES1", "NQ1", "YM1"},  // Example futures contracts
            std::chrono::system_clock::now() - std::chrono::hours(24),
            std::chrono::system_clock::now(),
            AssetClass::FUTURES,
            DataFrequency::DAILY
        );
        if (data_result.is_error()) {
            throw std::runtime_error(data_result.error()->what());
        }

        // Convert Arrow table to Bars
        auto bars_result = DataConversionUtils::arrow_table_to_bars(data_result.value());
        if (bars_result.is_error()) {
            throw std::runtime_error(bars_result.error()->what());
        }
        std::vector<Bar> bars = bars_result.value();

        // Process market data
        auto process_result = portfolio_manager.process_market_data(bars);
        if (process_result.is_error()) {
            throw std::runtime_error(process_result.error()->what());
        }

        // Get portfolio positions
        auto positions = portfolio_manager.get_portfolio_positions();
        std::cout << "Current portfolio positions:\n";
        for (const auto& [symbol, pos] : positions) {
            std::cout << symbol << ": " << pos.quantity << " @ " 
                     << pos.average_price << "\n";
        }

        // Get required position changes
        auto changes = portfolio_manager.get_required_changes();
        std::cout << "\nRequired position changes:\n";
        for (const auto& [symbol, change] : changes) {
            std::cout << symbol << ": " << change << "\n";
        }

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}