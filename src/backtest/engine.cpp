// src/backtest/engine.cpp
#include "trade_ngin/backtest/engine.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/data/conversion_utils.hpp"
#include <algorithm>
#include <numeric>
#include <cmath>

namespace trade_ngin {
namespace backtest {

BacktestEngine::BacktestEngine(
    BacktestConfig config,
    std::shared_ptr<DatabaseInterface> db)
    : config_(std::move(config))
    , db_(std::move(db)) {
    
    if (config_.use_risk_management) {
        risk_manager_ = std::make_unique<RiskManager>(config_.risk_config);
    }
    
    if (config_.use_optimization) {
        optimizer_ = std::make_unique<DynamicOptimizer>(config_.opt_config);
    }
}

Result<BacktestResults> BacktestEngine::run(
    std::shared_ptr<StrategyInterface> strategy) {
    
    try {
        // Load historical market data
        auto data_result = load_market_data();
        if (data_result.is_error()) {
            return make_error<BacktestResults>(
                data_result.error()->code(),
                data_result.error()->what(),
                "BacktestEngine"
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
        auto init_result = strategy->initialize();
        if (init_result.is_error()) {
            return make_error<BacktestResults>(
                init_result.error()->code(),
                init_result.error()->what(),
                "BacktestEngine"
            );
        }

        // Start strategy
        auto start_result = strategy->start();
        if (start_result.is_error()) {
            return make_error<BacktestResults>(
                start_result.error()->code(),
                start_result.error()->what(),
                "BacktestEngine"
            );
        }

        // Process each bar
        for (const auto& bar : data_result.value()) {
            auto process_result = process_bar(
                bar, strategy, current_positions, equity_curve);
            
            if (process_result.is_error()) {
                return make_error<BacktestResults>(
                    process_result.error()->code(),
                    process_result.error()->what(),
                    "BacktestEngine"
                );
            }
        }

        // Stop strategy
        strategy->stop();

        // Calculate final results
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

        return Result<BacktestResults>(results);

    } catch (const std::exception& e) {
        return make_error<BacktestResults>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error running backtest: ") + e.what(),
            "BacktestEngine"
        );
    }
}

Result<void> BacktestEngine::process_bar(
    const Bar& bar,
    std::shared_ptr<StrategyInterface> strategy,
    std::unordered_map<std::string, Position>& current_positions,
    std::vector<std::pair<Timestamp, double>>& equity_curve) {
    
    try {
        // Pass market data to strategy
        auto data_result = strategy->on_data({bar});
        if (data_result.is_error()) {
            return data_result;
        }

        // Get updated positions from strategy
        const auto& new_positions = strategy->get_positions();

        // Process position changes
        for (const auto& [symbol, new_pos] : new_positions) {
            const auto& curr_pos = current_positions[symbol];
            
            if (std::abs(new_pos.quantity - curr_pos.quantity) > 1e-6) {
                // Create execution report for position change
                ExecutionReport exec;
                exec.symbol = symbol;
                exec.fill_time = bar.timestamp;
                exec.fill_price = apply_slippage(
                    bar.close,
                    new_pos.quantity - curr_pos.quantity,
                    new_pos.quantity > curr_pos.quantity ? Side::BUY : Side::SELL
                );
                exec.filled_quantity = std::abs(new_pos.quantity - curr_pos.quantity);
                exec.side = new_pos.quantity > curr_pos.quantity ? Side::BUY : Side::SELL;
                exec.commission = calculate_transaction_costs(exec);

                // Update position
                current_positions[symbol] = new_pos;

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
            portfolio_value += pos.quantity * bar.close;
        }

        // Update equity curve
        equity_curve.emplace_back(bar.timestamp, portfolio_value);

        // Apply risk management if enabled
        if (config_.use_risk_management && risk_manager_) {
            auto risk_result = risk_manager_->process_positions(current_positions);
            if (risk_result.is_error()) {
                return make_error<void>(
                    risk_result.error()->code(),
                    risk_result.error()->what(),
                    "BacktestEngine"
                );
            }

            // Scale positions if risk limits exceeded
            if (risk_result.value().risk_exceeded) {
                double scale = risk_result.value().recommended_scale;
                for (auto& [symbol, pos] : current_positions) {
                    pos.quantity *= scale;
                }
            }
        }

        // Apply optimization if enabled
        if (config_.use_optimization && optimizer_) {
            // Collect optimization inputs
            std::vector<double> current_pos;
            std::vector<double> target_pos;
            std::vector<double> costs;
            std::vector<double> weights;
            std::vector<std::vector<double>> covariance; // Simplified for example

            // Run optimization
            auto opt_result = optimizer_->optimize_single_period(
                current_pos, target_pos, costs, weights, covariance);
            
            if (opt_result.is_error()) {
                return make_error<void>(
                    opt_result.error()->code(),
                    opt_result.error()->what()
                );
            }

            // Apply optimized positions
            size_t idx = 0;
            for (auto& [symbol, pos] : current_positions) {
                pos.quantity = opt_result.value().optimized_positions[idx++];
            }
        }

        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error processing bar: ") + e.what(),
            "BacktestEngine"
        );
    }
}

Result<std::vector<Bar>> BacktestEngine::load_market_data() const {
    try {
        // Load data from database
        auto result = db_->get_market_data(
            config_.symbols,
            config_.start_date,
            config_.end_date,
            config_.asset_class,
            config_.data_freq
        );

        if (result.is_error()) {
            return make_error<std::vector<Bar>>(
                result.error()->code(),
                result.error()->what(),
                "BacktestEngine"
            );
        }

        // Convert Arrow table to Bars
        return DataConversionUtils::arrow_table_to_bars(result.value());

    } catch (const std::exception& e) {
        return make_error<std::vector<Bar>>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error loading market data: ") + e.what(),
            "BacktestEngine"
        );
    }
}

double BacktestEngine::calculate_transaction_costs(
    const ExecutionReport& execution) const {
    
    // Base commission
    double commission = execution.filled_quantity * 
                       config_.commission_rate;

    // Add market impact based on size (simplified model)
    double market_impact = execution.filled_quantity * 
                          execution.fill_price * 
                          0.0001;  // 1 basis point

    return commission + market_impact;
}

double BacktestEngine::apply_slippage(
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

BacktestResults BacktestEngine::calculate_metrics(
    const std::vector<std::pair<Timestamp, double>>& equity_curve,
    const std::vector<ExecutionReport>& executions) const {
    
    BacktestResults results;
    
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

    // Calculate Sharpe ratio (assuming 0% risk-free rate for simplicity)
    if (results.volatility > 0) {
        results.sharpe_ratio = (mean_return * 252.0) / results.volatility;
    }

    // Trading metrics
    results.total_trades = executions.size();
    
    double total_profit = 0.0;
    double total_loss = 0.0;
    int winning_trades = 0;
    
    for (const auto& exec : executions) {
        double pnl = exec.side == Side::BUY ? 
            -exec.fill_price * exec.filled_quantity :
            exec.fill_price * exec.filled_quantity;

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

    return results;
}

std::vector<std::pair<Timestamp, double>> BacktestEngine::calculate_drawdowns(
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

std::unordered_map<std::string, double> BacktestEngine::calculate_risk_metrics(
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

Result<void> BacktestEngine::save_results(
    const BacktestResults& results,
    const std::string& run_id) const {
    
    try {
        // Create SQL query to save results
        std::string query = 
            "INSERT INTO " + config_.results_db_schema + ".backtest_results "
            "(run_id, total_return, sharpe_ratio, sortino_ratio, max_drawdown, "
            "calmar_ratio, volatility, total_trades, win_rate, profit_factor, "
            "var_95, cvar_95, start_date, end_date) VALUES "
            "($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14)";
        
        // Execute query
        auto result = db_->execute_query(query);
        if (result.is_error()) {
            return make_error<void>(
                result.error()->code(),
                result.error()->what(),
                "BacktestEngine"
            );
        }

        // Save equity curve if enabled
        if (config_.store_trade_details) {
            query = "INSERT INTO " + config_.results_db_schema + ".equity_curve "
                   "(run_id, timestamp, equity) VALUES ($1, $2, $3)";
            
            for (const auto& [timestamp, equity] : results.equity_curve) {
                auto curve_result = db_->execute_query(query);
                if (curve_result.is_error()) {
                    WARN("Failed to save equity curve point: " + 
                         std::string(curve_result.error()->what()));
                }
            }
        }

        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::DATABASE_ERROR,
            std::string("Error saving backtest results: ") + e.what(),
            "BacktestEngine"
        );
    }
}

Result<BacktestResults> BacktestEngine::load_results(
    const std::string& run_id) const {
    
    try {
        // Query main results
        std::string query = 
            "SELECT * FROM " + config_.results_db_schema + ".backtest_results "
            "WHERE run_id = $1";
        
        auto result = db_->execute_query(query);
        if (result.is_error()) {
            return make_error<BacktestResults>(
                result.error()->code(),
                result.error()->what(),
                "BacktestEngine"
            );
        }

        // Get Arrow table result
        auto table = result.value();
        if (table->num_rows() == 0) {
            return make_error<BacktestResults>(
                ErrorCode::DATA_NOT_FOUND,
                "No results found for run_id: " + run_id,
                "BacktestEngine"
            );
        }

        // Initialize results
        BacktestResults results;

        // Extract scalar fields from first row
        auto numeric_arrays = {
            std::make_pair("total_return", &results.total_return),
            std::make_pair("sharpe_ratio", &results.sharpe_ratio),
            std::make_pair("sortino_ratio", &results.sortino_ratio),
            std::make_pair("max_drawdown", &results.max_drawdown),
            std::make_pair("calmar_ratio", &results.calmar_ratio),
            std::make_pair("volatility", &results.volatility),
            std::make_pair("win_rate", &results.win_rate),
            std::make_pair("profit_factor", &results.profit_factor),
            std::make_pair("avg_win", &results.avg_win),
            std::make_pair("avg_loss", &results.avg_loss),
            std::make_pair("max_win", &results.max_win),
            std::make_pair("max_loss", &results.max_loss),
            std::make_pair("var_95", &results.var_95),
            std::make_pair("cvar_95", &results.cvar_95),
            std::make_pair("beta", &results.beta),
            std::make_pair("correlation", &results.correlation),
            std::make_pair("downside_volatility", &results.downside_volatility)
        };

        for (const auto& [field_name, value_ptr] : numeric_arrays) {
            auto column = table->GetColumnByName(field_name);
            if (column && column->num_chunks() > 0) {
                auto array = std::static_pointer_cast<arrow::DoubleArray>(
                    column->chunk(0));
                if (!array->IsNull(0)) {
                    *value_ptr = array->Value(0);
                }
            }
        }

        // Extract integer fields
        auto int_arrays = {
            std::make_pair("total_trades", &results.total_trades)
        };

        for (const auto& [field_name, value_ptr] : int_arrays) {
            auto column = table->GetColumnByName(field_name);
            if (column && column->num_chunks() > 0) {
                auto array = std::static_pointer_cast<arrow::Int32Array>(
                    column->chunk(0));
                if (!array->IsNull(0)) {
                    *value_ptr = array->Value(0);
                }
            }
        }

        // Load equity curve if available
        if (config_.store_trade_details) {
            query = 
                "SELECT timestamp, equity FROM " + 
                config_.results_db_schema + ".equity_curve "
                "WHERE run_id = $1 "
                "ORDER BY timestamp";
            
            auto curve_result = db_->execute_query(query);
            if (curve_result.is_ok()) {
                auto curve_table = curve_result.value();
                auto timestamp_col = curve_table->GetColumnByName("timestamp");
                auto equity_col = curve_table->GetColumnByName("equity");

                if (timestamp_col && equity_col && 
                    timestamp_col->num_chunks() > 0 && 
                    equity_col->num_chunks() > 0) {
                    
                    auto timestamps = std::static_pointer_cast<arrow::TimestampArray>(
                        timestamp_col->chunk(0));
                    auto equity_values = std::static_pointer_cast<arrow::DoubleArray>(
                        equity_col->chunk(0));

                    results.equity_curve.reserve(timestamps->length());
                    for (int64_t i = 0; i < timestamps->length(); ++i) {
                        if (!timestamps->IsNull(i) && !equity_values->IsNull(i)) {
                            results.equity_curve.emplace_back(
                                std::chrono::system_clock::time_point(
                                    std::chrono::seconds(timestamps->Value(i))),
                                equity_values->Value(i)
                            );
                        }
                    }
                }
            }

            // Load trade executions
            query = 
                "SELECT * FROM " + config_.results_db_schema + ".trade_executions "
                "WHERE run_id = $1 "
                "ORDER BY timestamp";
            
            auto exec_result = db_->execute_query(query);
            if (exec_result.is_ok()) {
                auto exec_table = exec_result.value();
                
                // Extract execution data into results.executions vector
                auto extract_executions = [&results](
                    const std::shared_ptr<arrow::Table>& table) {
                    
                    auto symbol_col = table->GetColumnByName("symbol");
                    auto side_col = table->GetColumnByName("side");
                    auto qty_col = table->GetColumnByName("quantity");
                    auto price_col = table->GetColumnByName("price");
                    auto time_col = table->GetColumnByName("timestamp");
                    
                    if (!symbol_col || !side_col || !qty_col || 
                        !price_col || !time_col) {
                        return;
                    }

                    auto symbols = std::static_pointer_cast<arrow::StringArray>(
                        symbol_col->chunk(0));
                    auto sides = std::static_pointer_cast<arrow::StringArray>(
                        side_col->chunk(0));
                    auto quantities = std::static_pointer_cast<arrow::DoubleArray>(
                        qty_col->chunk(0));
                    auto prices = std::static_pointer_cast<arrow::DoubleArray>(
                        price_col->chunk(0));
                    auto timestamps = std::static_pointer_cast<arrow::TimestampArray>(
                        time_col->chunk(0));

                    for (int64_t i = 0; i < table->num_rows(); ++i) {
                        if (!symbols->IsNull(i) && !sides->IsNull(i) && 
                            !quantities->IsNull(i) && !prices->IsNull(i) && 
                            !timestamps->IsNull(i)) {
                            
                            ExecutionReport exec;
                            exec.symbol = symbols->GetString(i);
                            exec.side = sides->GetString(i) == "BUY" ? 
                                Side::BUY : Side::SELL;
                            exec.filled_quantity = quantities->Value(i);
                            exec.fill_price = prices->Value(i);
                            exec.fill_time = std::chrono::system_clock::time_point(
                                std::chrono::seconds(timestamps->Value(i)));
                            
                            results.executions.push_back(exec);
                        }
                    }
                };

                extract_executions(exec_table);
            }

            // Calculate drawdown curve from equity curve
            results.drawdown_curve = calculate_drawdowns(results.equity_curve);
        }

        return Result<BacktestResults>(results);

    } catch (const std::exception& e) {
        return make_error<BacktestResults>(
            ErrorCode::DATABASE_ERROR,
            std::string("Error loading backtest results: ") + e.what(),
            "BacktestEngine"
        );
    }
}

Result<std::unordered_map<std::string, double>> BacktestEngine::compare_results(
    const std::vector<BacktestResults>& results) {
    
    std::unordered_map<std::string, double> comparison;
    
    if (results.empty()) {
        return Result<std::unordered_map<std::string, double>>(comparison);
    }

    // Calculate comparative metrics
    double avg_return = 0.0;
    double avg_sharpe = 0.0;
    double best_return = results[0].total_return;
    double worst_return = results[0].total_return;
    
    for (const auto& result : results) {
        avg_return += result.total_return;
        avg_sharpe += result.sharpe_ratio;
        best_return = std::max(best_return, result.total_return);
        worst_return = std::min(worst_return, result.total_return);
    }

    comparison["average_return"] = avg_return / results.size();
    comparison["average_sharpe"] = avg_sharpe / results.size();
    comparison["best_return"] = best_return;
    comparison["worst_return"] = worst_return;
    comparison["return_range"] = best_return - worst_return;

    // Calculate consistency metrics
    double return_variance = 0.0;
    for (const auto& result : results) {
        double diff = result.total_return - comparison["average_return"];
        return_variance += diff * diff;
    }
    comparison["return_stddev"] = std::sqrt(return_variance / results.size());

    return Result<std::unordered_map<std::string, double>>(comparison);
}

} // namespace backtest
} // namespace trade_ngin