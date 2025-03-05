#include "trade_ngin/backtest/engine.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/data/credential_store.hpp"
#include "trade_ngin/backtest/transaction_cost_analysis.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include <iostream>
#include <iomanip>
#include <fstream>

/*
TO-DO:
    - Setup db credentials
    - Get metadata from postgres on contracts (size, margin, etc.)
    - Implement risk management
    - Implement optimization
    - Visualize results
    - Implement slippage model
    - Portfolio might have issues initial_capital vs. total_capital (% allocation vs $ allocation)
    - Use the full portfolio instead of just the strategy
    - Fix data access for strategies & TCA
    - Move trend_following & regime_detector to strategies
    - Fix warnings about 'localtime' and 'gmtime' being unsafe
*/

int main() {
    try {
        // 1. Initialize database connection
        auto credentials = std::make_shared<trade_ngin::CredentialStore>("./config.json");
        std::string username = credentials->get<std::string>("database", "username");
        std::string password = credentials->get<std::string>("database", "password");
        std::string host = credentials->get<std::string>("database", "host");
        std::string port = credentials->get<std::string>("database", "port");
        std::string db_name = credentials->get<std::string>("database", "name");

        auto db = std::make_shared<trade_ngin::PostgresDatabase>(
            "postgresql://" + username + ":" + password + "@" + host + ":" + port + "/" + db_name
        );

        auto connect_result = db->connect();
        if (connect_result.is_error()) {
            std::cerr << "Failed to connect to database: " << connect_result.error()->what() << std::endl;
            return 1;
        }
        
        // 2. Configure backtest parameters
        trade_ngin::backtest::BacktestConfig config;
        config.start_date = std::chrono::system_clock::now() - std::chrono::hours(24 * 365 * 3); // 3 years of data
        config.end_date = std::chrono::system_clock::now();
        config.asset_class = trade_ngin::AssetClass::FUTURES;
        config.data_freq = trade_ngin::DataFrequency::DAILY;
        
        auto symbols = db->get_symbols(trade_ngin::AssetClass::FUTURES);
        if (symbols.is_ok()) {
            config.symbols = symbols.value();
        } else {
            // Handle the error case
            throw std::runtime_error("Failed to get symbols");
        }
        
        std::cout << "Symbols: ";
        for (const auto& symbol : config.symbols) {
            std::cout << symbol << " ";
        }

        exit(0);
        
        config.initial_capital = 1000000.0;  // $1M
        config.commission_rate = 0.0005;     // 5 basis points
        config.slippage_model = 1.0;         // 1 basis point
        config.use_risk_management = true;
        config.use_optimization = true;
        
        // 3. Configure risk management
        config.risk_config.capital = config.initial_capital;
        config.risk_config.confidence_level = 0.99;
        config.risk_config.lookback_period = 252;
        config.risk_config.var_limit = 0.15;
        config.risk_config.jump_risk_limit = 0.10;
        config.risk_config.max_correlation = 0.7;
        config.risk_config.max_gross_leverage = 4.0;
        config.risk_config.max_net_leverage = 2.0;
        
        // 4. Configure optimization
        config.opt_config.tau = 1.0;
        config.opt_config.capital = config.initial_capital;
        config.opt_config.asymmetric_risk_buffer = 0.1;
        config.opt_config.cost_penalty_scalar = 10;
        config.opt_config.max_iterations = 100;
        config.opt_config.convergence_threshold = 1e-6;
        
        // 5. Initialize backtest engine
        auto engine = std::make_unique<trade_ngin::backtest::BacktestEngine>(config, db);

        // 6. Setup portfolio configuration
        trade_ngin::PortfolioConfig portfolio_config;
        portfolio_config.total_capital = config.initial_capital;
        portfolio_config.reserve_capital = config.initial_capital * 0.1; // 10% reserve
        portfolio_config.max_strategy_allocation = 1.0; // Only have one strategy currently
        portfolio_config.min_strategy_allocation = 0.1;
        portfolio_config.use_optimization = true;
        portfolio_config.use_risk_management = true;
        portfolio_config.opt_config = config.opt_config;
        portfolio_config.risk_config = config.risk_config;

        // 7. Create portfolio manager
        auto portfolio = std::make_shared<trade_ngin::PortfolioManager>(portfolio_config);

        // 8. Configure strategies
        
        // 8.1. Trend following strategy
        trade_ngin::StrategyConfig tf_config;
        tf_config.capital_allocation = config.initial_capital * 1.0; // 100% allocation
        tf_config.max_leverage = 4.0;
        tf_config.save_positions = true;
        tf_config.save_signals = true;
        tf_config.save_executions = true;
        
        // Add position limits
        for (const auto& symbol : config.symbols) {
            tf_config.position_limits[symbol] = 1000.0;  // Max 1000 units per symbol
            tf_config.trading_params[symbol] = 1.0;      // *CHANGE*: Contract size multiplier
            tf_config.costs[symbol] = config.commission_rate;
        }
        
        // Configure trend following parameters
        trade_ngin::TrendFollowingConfig trend_config;
        trend_config.weight = 1.0;
        trend_config.risk_target = 0.2;       // Target 20% annualized risk
        trend_config.idm = 2.5;               // Instrument diversification multiplier
        trend_config.use_position_buffering = true;
        
        // EMA window pairs
        trend_config.ema_windows = {
            {2, 8}, {4, 16}, {8, 32}, {16, 64}, {32, 128}, {64, 256}
        };
        
        trend_config.vol_lookback_short = 32;  // Short vol lookback
        trend_config.vol_lookback_long = 252;  // Long vol lookback
        
        // Define FDM (forecast diversification multiplier)
        trend_config.fdm = {
            {1, 1.0}, {2, 1.03}, {3, 1.08}, {4, 1.13}, {5, 1.19}, {6, 1.26}
        };
        
        // 9. Create and initialize the strategies
        auto tf_strategy = std::make_shared<trade_ngin::TrendFollowingStrategy>(
            "TREND_FOLLOWING", tf_config, trend_config, db);
        
        auto init_result = tf_strategy->initialize();
        if (init_result.is_error()) {
            std::cerr << "Failed to initialize strategy: " << init_result.error()->what() << std::endl;
            return 1;
        }
        
        // 10. Add strategies to the portfolio
        portfolio->add_strategy(tf_strategy, tf_config.capital_allocation, true, true);

        // 11. Run the backtest
        std::cout << "Starting backtest..." << std::endl;
        auto result = engine->run(tf_strategy); // *CHANGE*: Run the portfolio instead of the strategy
        
        if (result.is_error()) {
            std::cerr << "Backtest failed: " << result.error()->what() << std::endl;
            return 1;
        }
        
        // 12. Analyze and display results
        const auto& backtest_results = result.value();
        
        std::cout << "======= Backtest Results =======" << std::endl;
        std::cout << "Total Return: " << (backtest_results.total_return * 100.0) << "%" << std::endl;
        std::cout << "Sharpe Ratio: " << backtest_results.sharpe_ratio << std::endl;
        std::cout << "Sortino Ratio: " << backtest_results.sortino_ratio << std::endl;
        std::cout << "Max Drawdown: " << (backtest_results.max_drawdown * 100.0) << "%" << std::endl;
        std::cout << "Calmar Ratio: " << backtest_results.calmar_ratio << std::endl;
        std::cout << "Volatility: " << (backtest_results.volatility * 100.0) << "%" << std::endl;
        std::cout << "Win Rate: " << (backtest_results.win_rate * 100.0) << "%" << std::endl;
        std::cout << "Total Trades: " << backtest_results.total_trades << std::endl;

        // 13. Perform transaction cost analysis
        trade_ngin::backtest::TCAConfig tca_config;
        tca_config.pre_trade_window = std::chrono::minutes(5);
        tca_config.post_trade_window = std::chrono::minutes(5);
        tca_config.spread_factor = 1.0;
        tca_config.market_impact_coefficient = 1.0;
        tca_config.volatility_multiplier = 1.5;
        tca_config.use_arrival_price = true;
        tca_config.use_vwap = true;
        tca_config.use_twap = true;
        tca_config.calculate_opportunity_costs = true;
        tca_config.analyze_timing_costs = true;
        
        auto tca = std::make_unique<trade_ngin::backtest::TransactionCostAnalyzer>(tca_config);
        
        std::cout << "\n======= Transaction Cost Analysis =======" << std::endl;
        
        // Calculate total transaction costs
        double total_commission = 0.0;
        double total_market_impact = 0.0;
        double total_spread_cost = 0.0;
        double total_timing_cost = 0.0;
        
        // Load market data for TCA
        std::vector<trade_ngin::Bar> market_data;
        // In a real implementation, you would load the market data from the database
        // For this example, we'll use the data available in backtest_results.executions
        
        // Group executions by symbol for analysis
        std::unordered_map<std::string, std::vector<trade_ngin::ExecutionReport>> executions_by_symbol;
        for (const auto& exec : backtest_results.executions) {
            executions_by_symbol[exec.symbol].push_back(exec);
        }
        
        // Analyze executions by symbol
        for (const auto& [symbol, executions] : executions_by_symbol) {
            auto tca_result = tca->analyze_trade_sequence(executions, market_data);
            if (tca_result.is_ok()) {
                const auto& metrics = tca_result.value();
                std::cout << "Symbol: " << symbol << std::endl;
                std::cout << "  Commission: $" << metrics.commission << std::endl;
                std::cout << "  Spread Cost: $" << metrics.spread_cost << std::endl;
                std::cout << "  Market Impact: $" << metrics.market_impact << std::endl;
                std::cout << "  Timing Cost: $" << metrics.timing_cost << std::endl;
                std::cout << "  Participation Rate: " << metrics.participation_rate * 100.0 << "%" << std::endl;
                std::cout << "  Execution Time: " << metrics.execution_time.count() << "ms" << std::endl;
                
                total_commission += metrics.commission;
                total_market_impact += metrics.market_impact;
                total_spread_cost += metrics.spread_cost;
                total_timing_cost += metrics.timing_cost;
            }
        }
        
        // Display total transaction costs
        std::cout << "\nTotal Transaction Costs:" << std::endl;
        std::cout << "  Total Commission: $" << total_commission << std::endl;
        std::cout << "  Total Spread Cost: $" << total_spread_cost << std::endl;
        std::cout << "  Total Market Impact: $" << total_market_impact << std::endl;
        std::cout << "  Total Timing Cost: $" << total_timing_cost << std::endl;
        std::cout << "  Total Costs: $" << (total_commission + total_spread_cost + total_market_impact + total_timing_cost) << std::endl;
        std::cout << "  % of Total Return: " << ((total_commission + total_spread_cost + total_market_impact + total_timing_cost) / 
                                               (backtest_results.total_return * config.initial_capital)) * 100.0 << "%" << std::endl;
        
        // 14. Analyze portfolio performance
        std::cout << "\n======= Portfolio Analysis =======" << std::endl;
        
        // Get portfolio positions
        auto portfolio_positions = portfolio->get_portfolio_positions();
        
        // Calculate portfolio metrics
        double portfolio_value = config.initial_capital;
        for (const auto& [symbol, pos] : portfolio_positions) {
            // Assume we use the last price from backtest results
            double last_price = 0.0;
            if (!backtest_results.executions.empty()) {
                for (auto it = backtest_results.executions.rbegin(); it != backtest_results.executions.rend(); ++it) {
                    if (it->symbol == symbol) {
                        last_price = it->fill_price;
                        break;
                    }
                }
            }
            
            if (last_price > 0.0) {
                portfolio_value += pos.quantity * last_price;
                
                std::cout << "Symbol: " << symbol << std::endl;
                std::cout << "  Position: " << pos.quantity << " shares" << std::endl;
                std::cout << "  Average Price: $" << pos.average_price << std::endl;
                std::cout << "  Last Price: $" << last_price << std::endl;
                std::cout << "  P&L: $" << (last_price - pos.average_price) * pos.quantity << std::endl;
                std::cout << "  Weight: " << (pos.quantity * last_price / portfolio_value) * 100.0 << "%" << std::endl;
            }
        }
        
        std::cout << "\nFinal Portfolio Value: $" << portfolio_value << std::endl;
        std::cout << "Total Return: " << ((portfolio_value / config.initial_capital) - 1.0) * 100.0 << "%" << std::endl;
        
        // 15. Save results to database and CSV
        std::string run_id = "TF_PORTFOLIO_" + std::to_string(
            std::chrono::system_clock::now().time_since_epoch().count());
            
        auto save_result = engine->save_results(backtest_results, run_id);
        if (save_result.is_error()) {
            std::cerr << "Warning: Failed to save results to database: " << save_result.error()->what() << std::endl;
        } else {
            std::cout << "Results saved to database with ID: " << run_id << std::endl;
        }
        
        // Save equity curve to CSV
        std::ofstream equity_curve_file("equity_curve_" + run_id + ".csv");
        if (equity_curve_file.is_open()) {
            equity_curve_file << "Date,Equity\n";
            for (const auto& [timestamp, equity] : backtest_results.equity_curve) {
                // Convert timestamp to string
                auto time_t = std::chrono::system_clock::to_time_t(timestamp);
                std::tm tm = *std::localtime(&time_t);
                std::stringstream ss;
                ss << std::put_time(&tm, "%Y-%m-%d");
                
                equity_curve_file << ss.str() << "," << equity << "\n";
            }
            equity_curve_file.close();
            std::cout << "Equity curve saved to CSV file" << std::endl;
        }
        
        // Save trade list to CSV
        std::ofstream trades_file("trades_" + run_id + ".csv");
        if (trades_file.is_open()) {
            trades_file << "Symbol,Side,Quantity,Price,DateTime,Commission\n";
            for (const auto& exec : backtest_results.executions) {
                auto time_t = std::chrono::system_clock::to_time_t(exec.fill_time);
                std::tm tm = *std::localtime(&time_t);
                std::stringstream ss;
                ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
                
                trades_file << exec.symbol << ","
                          << (exec.side == trade_ngin::Side::BUY ? "BUY" : "SELL") << ","
                          << exec.filled_quantity << ","
                          << exec.fill_price << ","
                          << ss.str() << ","
                          << exec.commission << "\n";
            }
            trades_file.close();
            std::cout << "Trade list saved to CSV file" << std::endl;
        }
        
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "Unexpected error: " << e.what() << std::endl;
        return 1;
    }
}