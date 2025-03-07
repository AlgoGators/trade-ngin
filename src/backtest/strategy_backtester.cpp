// src/backtest/strategy_backtester.cpp
#include "trade_ngin/backtest/strategy_backtester.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/data/database_interface.hpp"
#include "trade_ngin/data/conversion_utils.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include <algorithm>
#include <numeric>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <chrono>

namespace {
std::string join(const std::vector<std::string>& elements, const std::string& delimiter) {
    std::ostringstream os;
    if (!elements.empty()) {
        os << elements[0];
        for (size_t i = 1; i < elements.size(); ++i) {
            os << delimiter << elements[i];
        }
    }
    return os.str();
}
} // anonymous namespace

namespace trade_ngin {
namespace backtest {

StrategyBacktester::StrategyBacktester(
    StrategyBacktestConfig config,
    std::shared_ptr<PostgresDatabase> db)
    : config_(std::move(config))
    , db_(std::move(db)) {

    // Initialize slippage model if configured
    if (config_.slippage_model > 0.0) {
        SpreadSlippageConfig slippage_config;
        slippage_config.min_spread_bps = config_.slippage_model;
        slippage_config.spread_multiplier = 1.2;
        slippage_config.market_impact_multiplier = 1.5;
        
        slippage_model_ = SlippageModelFactory::create_spread_model(slippage_config);
    }

    INFO("Strategy backtester initialized with " + 
         std::to_string(config_.symbols.size()) + " symbols and " +
         std::to_string(config_.initial_capital) + " initial capital");
}

Result<StrategyBacktestResults> StrategyBacktester::run(
    std::shared_ptr<StrategyInterface> strategy) {

    try {
        // Load historical market data
        auto data_result = load_market_data();
        if (data_result.is_error()) {
            ERROR("Failed to load market data: " + 
                  std::string(data_result.error()->what()));
            
            return make_error<StrategyBacktestResults>(
                data_result.error()->code(),
                data_result.error()->what(),
                "StrategyBacktester"
            );
        }

        // Initialize tracking variables
        std::vector<ExecutionReport> executions;
        std::unordered_map<std::string, Position> current_positions;
        std::vector<std::pair<Timestamp, double>> equity_curve;
        double current_equity = config_.initial_capital;

        // Initialize equity curve with starting point
        equity_curve.emplace_back(config_.start_date, current_equity);

        // Initialize strategy
        INFO("Initializing strategy for backtest");
        auto init_result = strategy->initialize();
        if (init_result.is_error()) {
            ERROR("Strategy initialization failed: " + 
                  std::string(init_result.error()->what()));
            
            return make_error<StrategyBacktestResults>(
                init_result.error()->code(),
                init_result.error()->what(),
                "StrategyBacktester"
            );
        }

        // Start strategy
        INFO("Starting strategy for backtest");
        auto start_result = strategy->start();
        if (start_result.is_error()) {
            ERROR("Strategy start failed: " + 
                  std::string(start_result.error()->what()));
            
            return make_error<StrategyBacktestResults>(
                start_result.error()->code(),
                start_result.error()->what(),
                "StrategyBacktester"
            );
        }

        // Process each bar
        INFO("Starting backtest simulation with " + 
             std::to_string(data_result.value().size()) + " bars");
        
        size_t processed_bars = 0;
        
        // Group bars by timestamp for realistic simulation
        std::map<Timestamp, std::vector<Bar>> bars_by_time;
        for (const auto& bar : data_result.value()) {
            bars_by_time[bar.timestamp].push_back(bar);
        }
        
        // Process bars in chronological order
        for (const auto& [timestamp, bars] : bars_by_time) {
            // Update slippage model if available
            if (slippage_model_) {
                for (const auto& bar : bars) {
                    slippage_model_->update(bar);
                }
            }
            
            // Process bars
            auto process_result = process_bar(
                bars, strategy, current_positions, executions, equity_curve);
            
            if (process_result.is_error()) {
                ERROR("Bar processing failed: " + 
                      std::string(process_result.error()->what()));
                
                return make_error<StrategyBacktestResults>(
                    process_result.error()->code(),
                    process_result.error()->what(),
                    "StrategyBacktester"
                );
            }
            
            processed_bars += bars.size();
            
            // Periodically log progress
            if (processed_bars % 1000 == 0) {
                INFO("Processed " + std::to_string(processed_bars) + " bars");
            }
        }

        // Stop strategy
        INFO("Backtest complete, stopping strategy");
        strategy->stop();

        // Calculate final results
        INFO("Calculating backtest metrics");
        auto results = calculate_metrics(equity_curve, executions);

        // Add position and execution history
        results.executions = std::move(executions);
        results.positions.reserve(current_positions.size());
        for (const auto& [_, pos] : current_positions) {
            results.positions.push_back(pos);
        }
        results.equity_curve = std::move(equity_curve);

        // Calculate drawdown curve
        results.drawdown_curve = calculate_drawdowns(results.equity_curve);
        
        INFO("Strategy backtest completed successfully");

        return Result<StrategyBacktestResults>(results);

    } catch (const std::exception& e) {
        ERROR("Unexpected error during backtest: " + std::string(e.what()));

        return make_error<StrategyBacktestResults>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error running strategy backtest: ") + e.what(),
            "StrategyBacktester"
        );
    }
}

Result<void> StrategyBacktester::process_bar(
    const std::vector<Bar>& bars,
    std::shared_ptr<StrategyInterface> strategy,
    std::unordered_map<std::string, Position>& current_positions,
    std::vector<ExecutionReport>& executions,
    std::vector<std::pair<Timestamp, double>>& equity_curve) {
    
    try {
        // Pass market data to strategy
        auto data_result = strategy->on_data(bars);
        if (data_result.is_error()) {
            return data_result;
        }

        // Get updated positions from strategy
        const auto& new_positions = strategy->get_positions();
        std::vector<ExecutionReport> period_executions;

        // Process position changes
        for (const auto& [symbol, new_pos] : new_positions) {
            const auto current_it = current_positions.find(symbol);
            double current_qty = (current_it != current_positions.end()) ? 
                current_it->second.quantity : 0.0;
            
            if (std::abs(new_pos.quantity - current_qty) > 1e-6) {
                // Find latest price for symbol
                double latest_price = 0.0;
                for (const auto& bar : bars) {
                    if (bar.symbol == symbol) {
                        latest_price = bar.close;
                        break;
                    }
                }
                
                if (latest_price == 0.0) {
                    continue; // Skip if price not available
                }
                
                // Calculate trade size
                double trade_size = new_pos.quantity - current_qty;
                Side side = trade_size > 0 ? Side::BUY : Side::SELL;
                
                // Apply slippage to price
                double fill_price;
                if (slippage_model_) {
                    // Find the bar for this symbol
                    std::optional<Bar> symbol_bar;
                    for (const auto& bar : bars) {
                        if (bar.symbol == symbol) {
                            symbol_bar = bar;
                            break;
                        }
                    }
                    
                    fill_price = slippage_model_->calculate_slippage(
                        latest_price, 
                        std::abs(trade_size), 
                        side,
                        symbol_bar
                    );
                } else {
                    // Apply basic slippage model
                    fill_price = apply_slippage(latest_price, std::abs(trade_size), side);
                }

                // Create execution report
                ExecutionReport exec;
                exec.order_id = "BT-" + std::to_string(equity_curve.size());
                exec.exec_id = "EX-" + std::to_string(equity_curve.size());
                exec.symbol = symbol;
                exec.side = side;
                exec.filled_quantity = std::abs(trade_size);
                exec.fill_price = fill_price;
                exec.fill_time = bars[0].timestamp; // Use timestamp of current batch
                exec.commission = calculate_transaction_costs(exec);
                exec.is_partial = false;

                // Update position
                current_positions[symbol] = new_pos;
                
                // Add to executions for this period
                executions.push_back(exec);
                period_executions.push_back(exec);

                // Notify strategy of fill
                auto fill_result = strategy->on_execution(exec);
                if (fill_result.is_error()) {
                    return fill_result;
                }
            }
        }

        // Calculate current portfolio value
        double portfolio_value = config_.initial_capital;
        for (const auto& [symbol, pos] : current_positions) {
            // Find latest price for symbol
            double latest_price = 0.0;
            for (const auto& bar : bars) {
                if (bar.symbol == symbol) {
                    latest_price = bar.close;
                    break;
                }
            }
            
            if (latest_price > 0.0) {
                portfolio_value += pos.quantity * latest_price;
            }
        }

        // Update equity curve
        if (!bars.empty()) {
            equity_curve.emplace_back(bars[0].timestamp, portfolio_value);
        }

        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error processing bar: ") + e.what(),
            "StrategyBacktester"
        );
    }
}

Result<std::vector<Bar>> StrategyBacktester::load_market_data() const {
    try {
        INFO("Loading market data for backtest from " + 
            std::to_string(config_.start_date.time_since_epoch().count()) + 
             " to " + std::to_string(config_.end_date.time_since_epoch().count()));
        
        // Validate the database connection
        if (!db_) {
            return make_error<std::vector<Bar>>(
                ErrorCode::DATABASE_ERROR,
                "Database interface is null",
                "StrategyBacktester"
            );
        }
        
        if (!db_->is_connected()) {
            auto connect_result = db_->connect();
            if (connect_result.is_error()) {
                return make_error<std::vector<Bar>>(
                    connect_result.error()->code(),
                    "Failed to connect to database: " + std::string(connect_result.error()->what()),
                    "StrategyBacktester"
                );
            }
        }

        // Load market data directly using PostgresInterface
        auto result = db_->get_market_data(
            config_.symbols,
            config_.start_date,
            config_.end_date,
            config_.asset_class,
            config_.data_freq,
            config_.data_type
        );

        if (result.is_error()) {
            return make_error<std::vector<Bar>>(
                result.error()->code(),
                result.error()->what(),
                "StrategyBacktester"
            );
        }

        // Convert Arrow table to Bars using your DataConversionUtils
        auto conversion_result = DataConversionUtils::arrow_table_to_bars(result.value());
        if (conversion_result.is_error()) {
            return make_error<std::vector<Bar>>(
                conversion_result.error()->code(),
                conversion_result.error()->what(),
                "StrategyBacktester"
            );
        }
        
        auto& bars = conversion_result.value();
        INFO("Loaded " + std::to_string(bars.size()) + " bars for " + 
             std::to_string(config_.symbols.size()) + " symbols");
             
        return conversion_result;

    } catch (const std::exception& e) {
        return make_error<std::vector<Bar>>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error loading market data: ") + e.what(),
            "StrategyBacktester"
        );
    }
}

double StrategyBacktester::calculate_transaction_costs(
    const ExecutionReport& execution) const {
    
    // Base commission
    double commission = execution.filled_quantity * execution.fill_price * config_.commission_rate;

    // Add market impact based on size (simplified model)
    double market_impact = execution.filled_quantity * execution.fill_price * 0.0005;  // 5 basis points
    double fixed_cost = 1.0;  // Fixed cost per trade

    return commission + market_impact + fixed_cost;
}

double StrategyBacktester::apply_slippage(
    double price,
    double quantity,
    Side side) const {
    
    // Apply basic slippage model
    double slip_factor = config_.slippage_model / 10000.0;  // Convert bps to decimal
    
    if (side == Side::BUY) {
        return price * (1.0 + slip_factor);
    } else {
        return price * (1.0 - slip_factor);
    }
}

StrategyBacktestResults StrategyBacktester::calculate_metrics(
    const std::vector<std::pair<Timestamp, double>>& equity_curve,
    const std::vector<ExecutionReport>& executions) const {
    
    StrategyBacktestResults results;
    
    if (equity_curve.empty()) return results;

    // Calculate returns
    std::vector<double> returns;
    returns.reserve(equity_curve.size() - 1);
    
    for (size_t i = 1; i < equity_curve.size(); ++i) {
        double ret = (equity_curve[i].second - equity_curve[i-1].second) / 
                    equity_curve[i-1].second;
        returns.push_back(ret);
    }

    // Basic performance metrics
    results.total_return = (equity_curve.back().second - equity_curve.front().second) / 
                          equity_curve.front().second;

    // Calculate volatility
    double mean_return = std::accumulate(returns.begin(), returns.end(), 0.0) / 
                        returns.size();
    
    double sq_sum = std::inner_product(
        returns.begin(), returns.end(), returns.begin(), 0.0);
    results.volatility = std::sqrt(sq_sum / returns.size() - mean_return * mean_return) * 
                        std::sqrt(252.0);  // Annualize

    // Calculate Sharpe ratio (assuming 0% risk-free)
    if (results.volatility > 0) {
        results.sharpe_ratio = (mean_return * 252.0) / results.volatility;
    }

    // Calculate Sortino ratio
    double downside_sum = 0.0;
    int downside_count = 0;
    for (double ret : returns) {
        if (ret < 0) {
            downside_sum += ret * ret;
            downside_count++;
        }
    }

    double downside_dev = downside_count > 0 ? 
        std::sqrt(downside_sum / downside_count) * std::sqrt(252.0) : 1e-6;

    results.sortino_ratio = (mean_return * 252.0) / downside_dev;

    // Trading metrics
    results.total_trades = static_cast<int>(executions.size());
    
    double total_profit = 0.0;
    double total_loss = 0.0;
    int winning_trades = 0;
    
    for (const auto& exec : executions) {
        double pnl = exec.side == Side::BUY ? 
            -exec.fill_price * exec.filled_quantity - exec.commission :
            exec.fill_price * exec.filled_quantity - exec.commission;

        if (pnl > 0) {
            total_profit += pnl;
            winning_trades++;
            results.max_win = std::max(results.max_win, pnl);
        } else {
            total_loss -= pnl;
            results.max_loss = std::max(results.max_loss, -pnl);
        }
    }

    if (results.total_trades > 0) {
        results.win_rate = static_cast<double>(winning_trades) / results.total_trades;
        results.avg_win = winning_trades > 0 ? total_profit / winning_trades : 0.0;
        results.avg_loss = (results.total_trades - winning_trades) > 0 ? 
            total_loss / (results.total_trades - winning_trades) : 0.0;
    }

    if (total_loss > 0) {
        results.profit_factor = total_profit / total_loss;
    }

    // Calculate drawdown metrics
    auto drawdowns = calculate_drawdowns(equity_curve);
    if (!drawdowns.empty()) {
        results.max_drawdown = std::max_element(
            drawdowns.begin(),
            drawdowns.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; }
        )->second;
    }

    // Calculate Calmar ratio
    if (results.max_drawdown > 0) {
        results.calmar_ratio = results.total_return / results.max_drawdown;
    }

    // Calculate risk metrics
    auto risk_metrics = calculate_risk_metrics(returns);
    results.var_95 = risk_metrics["var_95"];
    results.cvar_95 = risk_metrics["cvar_95"];
    results.downside_volatility = risk_metrics["downside_volatility"];

    // Calculate monthly returns
    std::map<std::string, double> monthly_returns_map;
    for (size_t i = 1; i < equity_curve.size(); ++i) {
        auto time_t = std::chrono::system_clock::to_time_t(equity_curve[i].first);
        std::tm tm = *std::localtime(&time_t);
        
        std::ostringstream month_key;
        month_key << std::setw(4) << (tm.tm_year + 1900) << "-" 
                  << std::setw(2) << std::setfill('0') << (tm.tm_mon + 1);
        
        double period_return = (equity_curve[i].second - equity_curve[i-1].second) / 
                             equity_curve[i-1].second;
        
        monthly_returns_map[month_key.str()] += period_return;
    }
    
    for (const auto& [month, ret] : monthly_returns_map) {
        results.monthly_returns[month] = ret;
    }
    
    // Calculate per-symbol P&L
    std::map<std::string, double> symbol_pnl_map;
    for (const auto& exec : executions) {
        double trade_pnl = exec.side == Side::BUY ? 
            -exec.fill_price * exec.filled_quantity - exec.commission :
            exec.fill_price * exec.filled_quantity - exec.commission;
            
        symbol_pnl_map[exec.symbol] += trade_pnl;
    }
    
    for (const auto& [symbol, pnl] : symbol_pnl_map) {
        results.symbol_pnl[symbol] = pnl;
    }

    return results;
}

std::vector<std::pair<Timestamp, double>> StrategyBacktester::calculate_drawdowns(
    const std::vector<std::pair<Timestamp, double>>& equity_curve) const {
    
    std::vector<std::pair<Timestamp, double>> drawdowns;
    drawdowns.reserve(equity_curve.size());

    if (equity_curve.empty()) return drawdowns;

    double peak = equity_curve[0].second;
    
    for (const auto& [timestamp, equity] : equity_curve) {
        peak = std::max(peak, equity);
        double drawdown = equity < peak ? (peak - equity) / peak : 0.0;
        drawdowns.emplace_back(timestamp, drawdown);
    }

    return drawdowns;
}

std::unordered_map<std::string, double> StrategyBacktester::calculate_risk_metrics(
    const std::vector<double>& returns) const {
    
    std::unordered_map<std::string, double> metrics;
    
    if (returns.empty()) return metrics;

    // Sort returns for percentile calculations
    std::vector<double> sorted_returns = returns;
    std::sort(sorted_returns.begin(), sorted_returns.end());

    // Calculate VaR 95%
    size_t var_index = static_cast<size_t>(returns.size() * 0.05);
    metrics["var_95"] = -sorted_returns[var_index];

    // Calculate CVaR 95%
    double cvar_sum = 0.0;
    for (size_t i = 0; i < var_index; ++i) {
        cvar_sum += sorted_returns[i];
    }
    metrics["cvar_95"] = -cvar_sum / var_index;

    // Calculate downside volatility
    double downside_sum = 0.0;
    int downside_count = 0;
    
    for (double ret : returns) {
        if (ret < 0) {
            downside_sum += ret * ret;
            downside_count++;
        }
    }
    
    metrics["downside_volatility"] = downside_count > 0 ?
        std::sqrt(downside_sum / downside_count) * std::sqrt(252.0) : 0.0;

    return metrics;
}

} // namespace backtest
} // namespace trade_ngin