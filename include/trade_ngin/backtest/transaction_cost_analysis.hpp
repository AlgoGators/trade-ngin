#pragma once

#include <chrono>
#include <memory>
#include <unordered_map>
#include <vector>
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"

namespace trade_ngin {
namespace backtest {

/**
 * @brief Detailed transaction cost breakdown
 */
struct TransactionCostMetrics {
    double commission{0.0};        // Fixed and percentage commissions
    double spread_cost{0.0};       // Cost from bid-ask spread
    double market_impact{0.0};     // Price impact of the trade
    double delay_cost{0.0};        // Implementation shortfall from delays
    double timing_cost{0.0};       // Cost of trading at suboptimal times
    double opportunity_cost{0.0};  // Cost of missed trades

    // Additional metrics
    double participation_rate{0.0};               // Trade volume / Market volume
    double price_reversion{0.0};                  // Post-trade price movement
    std::chrono::milliseconds execution_time{0};  // Time to complete
    int num_child_orders{0};                      // Number of child orders
};

/**
 * @brief Configuration for TCA analysis
 */
struct TCAConfig {
    // Analysis windows
    std::chrono::minutes pre_trade_window{5};
    std::chrono::minutes post_trade_window{5};

    // Cost calculation parameters
    double spread_factor{1.0};
    double market_impact_coefficient{1.0};
    double volatility_multiplier{1.5};

    // Benchmarks to use
    bool use_arrival_price{true};
    bool use_vwap{true};
    bool use_twap{true};

    // Additional analysis
    bool calculate_opportunity_costs{true};
    bool analyze_timing_costs{true};
    int max_child_orders_analyzed{100};
};

/**
 * @brief Transaction Cost Analysis engine
 */
class TransactionCostAnalyzer {
public:
    explicit TransactionCostAnalyzer(TCAConfig config);

    /**
     * @brief Analyze transaction costs for a trade
     * @param execution Trade execution details
     * @param market_data Market data around trade time
     * @return Detailed cost analysis
     */
    Result<TransactionCostMetrics> analyze_trade(const ExecutionReport& execution,
                                                 const std::vector<Bar>& market_data) const;

    /**
     * @brief Analyze costs for a series of related trades
     * @param executions Vector of related executions
     * @param market_data Market data covering trade period
     * @return Aggregated cost analysis
     */
    Result<TransactionCostMetrics> analyze_trade_sequence(
        const std::vector<ExecutionReport>& executions, const std::vector<Bar>& market_data) const;

    /**
     * @brief Calculate implementation shortfall
     * @param target_position Intended position
     * @param actual_executions Actual trade executions
     * @param market_data Market data for analysis
     * @return Implementation shortfall metrics
     */
    Result<TransactionCostMetrics> calculate_implementation_shortfall(
        const Position& target_position, const std::vector<ExecutionReport>& actual_executions,
        const std::vector<Bar>& market_data) const;

    /**
     * @brief Analyze execution quality vs benchmarks
     * @param executions Trade executions
     * @param market_data Market data
     * @return Map of benchmark name to performance vs benchmark
     */
    Result<std::unordered_map<std::string, double>> analyze_benchmark_performance(
        const std::vector<ExecutionReport>& executions, const std::vector<Bar>& market_data) const;

    /**
     * @brief Generate TCA report
     * @param metrics Cost metrics
     * @param include_charts Whether to include charts
     * @return Formatted report as string
     */
    std::string generate_report(const TransactionCostMetrics& metrics,
                                bool include_charts = true) const;

private:
    TCAConfig config_;

    /**
     * @brief Calculate spread costs
     */
    double calculate_spread_cost(const ExecutionReport& execution,
                                 const std::vector<Bar>& market_data) const;

    /**
     * @brief Calculate market impact
     */
    double calculate_market_impact(const ExecutionReport& execution,
                                   const std::vector<Bar>& market_data) const;

    /**
     * @brief Calculate timing costs
     */
    double calculate_timing_cost(const ExecutionReport& execution,
                                 const std::vector<Bar>& market_data) const;

    /**
     * @brief Calculate opportunity costs
     */
    double calculate_opportunity_cost(const Position& target_position,
                                      const std::vector<ExecutionReport>& actual_executions,
                                      const std::vector<Bar>& market_data) const;
};

}  // namespace backtest
}  // namespace trade_ngin