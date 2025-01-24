#include "trade_ngin/strategy/trend_following.hpp"
#include "trade_ngin/backtest/engine.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include <iostream>
#include <chrono>
#include <iomanip>

using namespace trade_ngin;
using namespace trade_ngin::backtest;

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

        // Configure strategy
        StrategyConfig strategy_config;
        strategy_config.capital_allocation = 1000000.0;  // $1M initial capital
        strategy_config.max_leverage = 2.0;
        strategy_config.asset_classes = {AssetClass::FUTURES};
        strategy_config.frequencies = {DataFrequency::DAILY};
        strategy_config.save_signals = true;
        strategy_config.save_positions = true;
        strategy_config.trading_params = {
            {"ES", 50.0},   // E-mini S&P multiplier
            {"NQ", 20.0},   // E-mini Nasdaq multiplier
            {"YM", 5.0}     // E-mini Dow multiplier
        };

        // Configure trend following parameters
        TrendFollowingConfig trend_config;
        trend_config.risk_target = 0.20;        // 20% annualized vol target
        trend_config.idm = 2.5;                 // Instrument diversification multiplier
        trend_config.use_position_buffering = true;
        trend_config.ema_windows = {
            {2, 8}, {4, 16}, {8, 32}, {16, 64}, {32, 128}
        };
        trend_config.vol_lookback_short = 22;   // 1 month
        trend_config.vol_lookback_long = 252;   // 1 year

        // Create strategy instance
        auto strategy = std::make_shared<TrendFollowingStrategy>(
            "TREND_1",
            strategy_config,
            trend_config,
            db
        );

        // Configure backtest
        BacktestConfig backtest_config;
        backtest_config.start_date = std::chrono::system_clock::from_time_t(
            std::chrono::system_clock::to_time_t(
                std::chrono::system_clock::now() - std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::hours(24 * 365))
            )
        );
        backtest_config.end_date = std::chrono::system_clock::now();
        backtest_config.symbols = {"ES", "NQ", "YM"};  // Major equity futures
        backtest_config.asset_class = AssetClass::FUTURES;
        backtest_config.data_freq = DataFrequency::DAILY;
        backtest_config.initial_capital = 1000000.0;
        backtest_config.reinvest_profits = true;
        backtest_config.commission_rate = 0.0001;  // 1 bps commission
        backtest_config.slippage_model = 0.0001;   // 1 bps slippage

        // Configure risk management
        backtest_config.risk_config.portfolio_var_limit = 0.15;     // 15% VaR limit
        backtest_config.risk_config.max_drawdown = 0.20;           // 20% max drawdown
        backtest_config.risk_config.max_correlation = 0.7;         // 70% correlation limit
        backtest_config.risk_config.max_gross_leverage = 4.0;      // 4x max leverage
        backtest_config.risk_config.capital = backtest_config.initial_capital;
        backtest_config.use_risk_management = true;

        // Configure optimization
        backtest_config.opt_config.tau = 1.0;                     // Risk aversion
        backtest_config.opt_config.asymmetric_risk_buffer = 0.1;  // 10% buffer
        backtest_config.opt_config.cost_penalty_scalar = 10;      // Trading cost penalty
        backtest_config.use_optimization = true;

        // Create and run backtest
        BacktestEngine engine(backtest_config, db);
        auto result = engine.run(strategy);
        if (result.is_error()) {
            throw std::runtime_error(result.error()->what());
        }

        // Print results
        const auto& metrics = result.value();
        std::cout << std::fixed << std::setprecision(4)
                  << "\nBacktest Results:\n"
                  << "================\n"
                  << "Total Return: " << (metrics.total_return * 100) << "%\n"
                  << "Sharpe Ratio: " << metrics.sharpe_ratio << "\n"
                  << "Sortino Ratio: " << metrics.sortino_ratio << "\n"
                  << "Max Drawdown: " << (metrics.max_drawdown * 100) << "%\n"
                  << "Calmar Ratio: " << metrics.calmar_ratio << "\n"
                  << "Win Rate: " << (metrics.win_rate * 100) << "%\n"
                  << "Profit Factor: " << metrics.profit_factor << "\n"
                  << "Total Trades: " << metrics.total_trades << "\n"
                  << "\nRisk Metrics:\n"
                  << "VaR (95%): " << (metrics.var_95 * 100) << "%\n"
                  << "CVaR (95%): " << (metrics.cvar_95 * 100) << "%\n"
                  << "Annual Volatility: " << (metrics.volatility * 100) << "%\n";

        // Save results to database
        auto save_result = engine.save_results(metrics, "TREND_1_" + 
            std::to_string(std::chrono::system_clock::to_time_t(
                std::chrono::system_clock::now())));
            
        if (save_result.is_error()) {
            std::cerr << "Warning: Failed to save results: " 
                     << save_result.error()->what() << "\n";
        }

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}