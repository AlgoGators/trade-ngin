// src/backtest/transaction_cost_analysis.cpp
#include "trade_ngin/backtest/transaction_cost_analysis.hpp"
#include "trade_ngin/core/logger.hpp"
#include <algorithm>
#include <numeric>
#include <sstream>
#include <iomanip>

namespace trade_ngin {
namespace backtest {

TransactionCostAnalyzer::TransactionCostAnalyzer(TCAConfig config)
    : config_(std::move(config)) {}

Result<TransactionCostMetrics> TransactionCostAnalyzer::analyze_trade(
    const ExecutionReport& execution,
    const std::vector<Bar>& market_data) const {
    
    try {
        TransactionCostMetrics metrics;

        // Calculate spread cost
        metrics.spread_cost = calculate_spread_cost(execution, market_data);

        // Calculate market impact
        metrics.market_impact = calculate_market_impact(execution, market_data);

        // Calculate timing cost
        metrics.timing_cost = calculate_timing_cost(execution, market_data);

        // Calculate participation rate
        auto market_volume_it = std::find_if(
            market_data.begin(), market_data.end(),
            [&execution](const Bar& bar) {
                return bar.symbol == execution.symbol &&
                       std::abs(std::chrono::duration_cast<std::chrono::minutes>(
                           bar.timestamp - execution.fill_time).count()) < 5;
            });

        if (market_volume_it != market_data.end()) {
            metrics.participation_rate = execution.filled_quantity.as_double() / 
                                      market_volume_it->volume;
        }

        // Calculate price reversion
        if (!market_data.empty() && market_volume_it != market_data.end()) {
            auto post_trade_it = std::find_if(
                market_volume_it, market_data.end(),
                [&execution](const Bar& bar) {
                    return std::chrono::duration_cast<std::chrono::minutes>(
                        bar.timestamp - execution.fill_time).count() >= 
                        30;  // Look 30 minutes ahead
                });

            if (post_trade_it != market_data.end()) {
                metrics.price_reversion = 
                    (post_trade_it->close.as_double() - execution.fill_price.as_double()) / 
                    execution.fill_price.as_double();
            }
        }

        return Result<TransactionCostMetrics>(metrics);

    } catch (const std::exception& e) {
        return make_error<TransactionCostMetrics>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error analyzing trade: ") + e.what(),
            "TransactionCostAnalyzer"
        );
    }
}

Result<TransactionCostMetrics> TransactionCostAnalyzer::analyze_trade_sequence(
    const std::vector<ExecutionReport>& executions,
    const std::vector<Bar>& market_data) const {
    
    try {
        TransactionCostMetrics aggregate_metrics;
        double total_value = 0.0;

        // Analyze each execution and weight the metrics by trade value
        for (const auto& execution : executions) {
            auto result = analyze_trade(execution, market_data);
            if (result.is_error()) {
                return result;
            }

            double trade_value = std::abs(execution.filled_quantity.as_double() * 
                                        execution.fill_price.as_double());
            total_value += trade_value;

            const auto& metrics = result.value();
            aggregate_metrics.spread_cost += metrics.spread_cost * trade_value;
            aggregate_metrics.market_impact += metrics.market_impact * trade_value;
            aggregate_metrics.timing_cost += metrics.timing_cost * trade_value;
            aggregate_metrics.delay_cost += metrics.delay_cost * trade_value;
        }

        // Normalize weighted metrics
        if (total_value > 0) {
            aggregate_metrics.spread_cost /= total_value;
            aggregate_metrics.market_impact /= total_value;
            aggregate_metrics.timing_cost /= total_value;
            aggregate_metrics.delay_cost /= total_value;
        }

        // Set execution statistics
        aggregate_metrics.num_child_orders = static_cast<int>(executions.size());
        if (!executions.empty()) {
            aggregate_metrics.execution_time = 
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    executions.back().fill_time - executions.front().fill_time);
        }

        return Result<TransactionCostMetrics>(aggregate_metrics);

    } catch (const std::exception& e) {
        return make_error<TransactionCostMetrics>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error analyzing trade sequence: ") + e.what(),
            "TransactionCostAnalyzer"
        );
    }
}

Result<TransactionCostMetrics> TransactionCostAnalyzer::calculate_implementation_shortfall(
    const Position& target_position,
    const std::vector<ExecutionReport>& actual_executions,
    const std::vector<Bar>& market_data) const {
    
    try {
        TransactionCostMetrics metrics;

        // Find arrival price (price when decision was made)
        double arrival_price = 0.0;
        if (!market_data.empty()) {
            auto it = std::lower_bound(
                market_data.begin(),
                market_data.end(),
                target_position.last_update,
                [](const Bar& bar, const Timestamp& ts) {
                    return bar.timestamp < ts;
                });

            if (it != market_data.end()) {
                arrival_price = it->close.as_double();
            }
        }

        if (arrival_price > 0.0) {
            // Calculate VWAP of executions
            double total_value = 0.0;
            double total_quantity = 0.0;
            
            for (const auto& exec : actual_executions) {
                total_value += exec.fill_price.as_double() * exec.filled_quantity.as_double();
                total_quantity += exec.filled_quantity.as_double();
            }

            double vwap = total_quantity > 0.0 ? 
                         total_value / total_quantity : 0.0;

            // Calculate implementation shortfall components
            if (target_position.quantity.as_double() > 0) {  // Buying
                metrics.delay_cost = vwap - arrival_price;
            } else {  // Selling
                metrics.delay_cost = arrival_price - vwap;
            }

            // Calculate opportunity cost for unfilled portion
            double unfilled_quantity = target_position.quantity.as_double();
            for (const auto& exec : actual_executions) {
                unfilled_quantity -= exec.filled_quantity.as_double();
            }

            if (std::abs(unfilled_quantity) > 0.0 && !market_data.empty()) {
                double final_price = market_data.back().close.as_double();
                if (unfilled_quantity > 0) {  // Missed buy
                    metrics.opportunity_cost = final_price - arrival_price;
                } else {  // Missed sell
                    metrics.opportunity_cost = arrival_price - final_price;
                }
                metrics.opportunity_cost *= std::abs(unfilled_quantity);
            }
        }

        return Result<TransactionCostMetrics>(metrics);

    } catch (const std::exception& e) {
        return make_error<TransactionCostMetrics>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error calculating implementation shortfall: ") + e.what(),
            "TransactionCostAnalyzer"
        );
    }
}

Result<std::unordered_map<std::string, double>> 
TransactionCostAnalyzer::analyze_benchmark_performance(
    const std::vector<ExecutionReport>& executions,
    const std::vector<Bar>& market_data) const {
    
    try {
        std::unordered_map<std::string, double> benchmark_metrics;

        if (executions.empty() || market_data.empty()) {
            return Result<std::unordered_map<std::string, double>>(benchmark_metrics);
        }

        // Calculate VWAP of executions
        double total_value = 0.0;
        double total_quantity = 0.0;
        
        for (const auto& exec : executions) {
            total_value += exec.fill_price.as_double() * exec.filled_quantity.as_double();
            total_quantity += exec.filled_quantity.as_double();
        }

        double execution_vwap = total_quantity > 0.0 ? 
                              total_value / total_quantity : 0.0;

        // Calculate market VWAP
        if (config_.use_vwap) {
            double market_value = 0.0;
            double market_volume = 0.0;
            
            // Calculate market VWAP during execution period
            auto start_time = executions.front().fill_time;
            auto end_time = executions.back().fill_time;
            
            for (const auto& bar : market_data) {
                if (bar.timestamp >= start_time && bar.timestamp <= end_time) {
                    market_value += bar.close.as_double() * bar.volume;
                    market_volume += bar.volume;
                }
            }

            double market_vwap = market_volume > 0.0 ? 
                               market_value / market_volume : 0.0;

            benchmark_metrics["vwap_performance"] = 
                (execution_vwap - market_vwap) / market_vwap;
        }

        // Calculate TWAP benchmark
        if (config_.use_twap) {
            double twap = 0.0;
            int count = 0;
            
            for (const auto& bar : market_data) {
                if (bar.timestamp >= executions.front().fill_time &&
                    bar.timestamp <= executions.back().fill_time) {
                    twap += bar.close.as_double();
                    count++;
                }
            }

            if (count > 0) {
                twap /= count;
                benchmark_metrics["twap_performance"] = 
                    (execution_vwap - twap) / twap;
            }
        }

        // Calculate arrival price performance
        if (config_.use_arrival_price) {
            auto arrival_price = market_data.front().close.as_double();  // First price in window
            benchmark_metrics["arrival_price_performance"] = 
                (execution_vwap - arrival_price) / arrival_price;
        }

        return Result<std::unordered_map<std::string, double>>(benchmark_metrics);

    } catch (const std::exception& e) {
        return make_error<std::unordered_map<std::string, double>>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error analyzing benchmark performance: ") + e.what(),
            "TransactionCostAnalyzer"
        );
    }
}

double TransactionCostAnalyzer::calculate_spread_cost(
    const ExecutionReport& execution,
    const std::vector<Bar>& market_data) const {
    
    // Find the closest bar before execution
    auto it = std::lower_bound(
        market_data.begin(),
        market_data.end(),
        execution.fill_time,
        [](const Bar& bar, const Timestamp& ts) {
            return bar.timestamp < ts;
        });

    if (it == market_data.begin()) {
        return 0.0; // No bars before execution
    }

    if (it == market_data.end()) {
        // Use the last bar if all bars are before execution
        --it;
    } else {
        // Use the previous bar
        --it;
    }

    double spread_estimate = (it->high.as_double() - it->low.as_double()) / it->close.as_double();
    return spread_estimate * 0.5 * execution.fill_price.as_double() * 
           std::abs(execution.filled_quantity.as_double());
}

double TransactionCostAnalyzer::calculate_market_impact(
    const ExecutionReport& execution,
    const std::vector<Bar>& market_data) const {
    
    // Find the bar immediately before execution
    auto it = std::lower_bound(
        market_data.begin(), market_data.end(), execution.fill_time,
        [](const Bar& bar, const Timestamp& ts) { return bar.timestamp < ts; });

    if (it == market_data.begin() || it == market_data.end()) {
        return 0.0; // Insufficient data
    }
    --it; // Move to the last bar before execution

    double pre_price = it->close.as_double();
    double market_move = 0.0;

    // Get the next bar to determine natural market movement
    auto next_it = it + 1;
    if (next_it != market_data.end()) {
        market_move = (next_it->close.as_double() - pre_price) / pre_price;
    }

    // Calculate market-adjusted price (price without the trade's impact)
    double market_adjusted_price = pre_price * (1 + market_move);
    
    // Compute price impact relative to market-adjusted price
    double price_impact = (execution.fill_price.as_double() - market_adjusted_price) / pre_price;

    // For BUY orders: Adverse impact if execution price > market-adjusted price
    // For SELL orders: Adverse impact if execution price < market-adjusted price
    if (execution.side == Side::BUY) {
        price_impact = std::max(price_impact, 0.0); // Only positive impacts
    } else {
        price_impact = std::max(-price_impact, 0.0); // Convert negative impacts to positive
    }

    return price_impact * execution.fill_price.as_double() * std::abs(execution.filled_quantity.as_double());
}

double TransactionCostAnalyzer::calculate_timing_cost(
    const ExecutionReport& execution,
    const std::vector<Bar>& market_data) const {
    
    // Find optimal execution price in window around trade
    auto window_start = execution.fill_time - 
                       config_.pre_trade_window;
    auto window_end = execution.fill_time + 
                     config_.post_trade_window;

    double best_price = execution.fill_price.as_double();  // Default to actual price
    
    for (const auto& bar : market_data) {
        if (bar.timestamp >= window_start && 
            bar.timestamp <= window_end) {
            if (execution.side == Side::BUY) {
                best_price = std::min(best_price, bar.low.as_double());
            } else {
                best_price = std::max(best_price, bar.high.as_double());
            }
        }
    }

    // Calculate timing cost as difference from optimal price
    return std::abs((execution.fill_price.as_double() - best_price) * 
                    execution.filled_quantity.as_double());
}

double TransactionCostAnalyzer::calculate_opportunity_cost(
    const Position& target_position,
    const std::vector<ExecutionReport>& actual_executions,
    const std::vector<Bar>& market_data) const {
    
    // Calculate unfilled quantity
    double unfilled = target_position.quantity.as_double();
    for (const auto& exec : actual_executions) {
        unfilled -= exec.filled_quantity.as_double();
    }

    if (std::abs(unfilled) < 1e-6 || market_data.empty()) {
        return 0.0;
    }

    // Calculate price movement since decision
    double start_price = market_data.front().close.as_double();
    double end_price = market_data.back().close.as_double();
    
    // Cost is the price movement on unfilled quantity
    if (unfilled > 0) {  // Missed buy
        return (end_price - start_price) * unfilled;
    } else {  // Missed sell
        return (start_price - end_price) * -unfilled;
    }
}

std::string TransactionCostAnalyzer::generate_report(
    const TransactionCostMetrics& metrics,
    bool include_charts) const {
    
    std::stringstream report;
    report << std::fixed << std::setprecision(4);
    
    report << "Transaction Cost Analysis Report\n"
           << "================================\n\n"
           << "Execution Costs:\n"
           << "  Spread Cost: " << metrics.spread_cost << "\n"
           << "  Market Impact: " << metrics.market_impact << "\n"
           << "  Timing Cost: " << metrics.timing_cost << "\n"
           << "  Delay Cost: " << metrics.delay_cost << "\n"
           << "  Total Cost: " << (metrics.spread_cost + 
                                  metrics.market_impact +
                                  metrics.timing_cost + 
                                  metrics.delay_cost) << "\n\n"
           << "Execution Statistics:\n"
           << "  Participation Rate: " << 
              (metrics.participation_rate * 100.0) << "%\n"
           << "  Number of Orders: " << metrics.num_child_orders << "\n"
           << "  Execution Time: " << 
              metrics.execution_time.count() << "ms\n"
           << "  Price Reversion: " << 
              (metrics.price_reversion * 100.0) << "%\n";

    if (metrics.opportunity_cost != 0.0) {
        report << "  Opportunity Cost: " << metrics.opportunity_cost << "\n";
    }

    return report.str();
}

} // namespace backtest
} // namespace trade_ngin