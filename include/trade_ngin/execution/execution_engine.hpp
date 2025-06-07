// include/trade_ngin/execution/execution_engine.hpp
#pragma once

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/order/order_manager.hpp"
#include "trade_ngin/core/config_base.hpp"
#include <memory>
#include <vector>
#include <unordered_map>
#include <chrono>

namespace trade_ngin {

/**
 * @brief Execution algorithm types
 */
enum class ExecutionAlgo {
    MARKET,          // Simple market orders
    TWAP,            // Time-weighted average price
    VWAP,            // Volume-weighted average price
    IS,              // Implementation shortfall
    POV,             // Percentage of volume
    DARK_POOL,       // Dark pool liquidity seeking
    ADAPTIVE_LIMIT,  // Adaptive limit order algorithm
    CUSTOM           // Custom algorithm
};

/**
 * @brief Execution metrics for analysis
 */
struct ExecutionMetrics {
    double participation_rate{0.0};     // Actual participation rate achieved
    double market_impact{0.0};          // Estimated market impact
    double implementation_shortfall{0.0}; // Implementation shortfall cost
    double arrival_price{0.0};          // Price at order arrival
    double vwap_price{0.0};             // VWAP during execution
    double twap_price{0.0};             // TWAP during execution
    double average_fill_price{0.0};     // Average execution price
    double volume_participation{0.0};    // Volume participation achieved
    std::chrono::milliseconds total_time{0}; // Total execution time
    int num_child_orders{0};            // Number of child orders generated
    double completion_rate{0.0};        // Percentage of order completed
};

/**
 * @brief Configuration for execution algorithms
 */
struct ExecutionConfig : public ConfigBase {
    double max_participation_rate{0.3};  // Maximum participation in volume
    double urgency_level{0.5};          // Urgency factor (0-1)
    std::chrono::minutes time_horizon{60}; // Time horizon for completion
    bool allow_cross_venue{true};       // Allow cross-venue execution
    bool dark_pool_only{false};         // Restrict to dark pools
    int max_child_orders{100};          // Maximum number of child orders
    double min_child_size{100.0};       // Minimum child order size
    std::vector<std::string> venues;    // Allowed execution venues
    std::unordered_map<std::string, double> venue_weights; // Venue routing weights

    // Configuration metadata
    std::string version{"1.0.0"};       // Configuration version

    // Implement serialization methods
    nlohmann::json to_json() const override {
        nlohmann::json j;
        j["max_participation_rate"] = max_participation_rate;
        j["urgency_level"] = urgency_level;
        j["time_horizon"] = time_horizon.count();
        j["allow_cross_venue"] = allow_cross_venue;
        j["dark_pool_only"] = dark_pool_only;
        j["max_child_orders"] = max_child_orders;
        j["min_child_size"] = min_child_size;
        j["venues"] = venues;
        j["venue_weights"] = venue_weights;
        j["version"] = version;

        return j;
    }

    void from_json(const nlohmann::json& j) override {
        if (j.contains("max_participation_rate")) max_participation_rate = j.at("max_participation_rate").get<double>();
        if (j.contains("urgency_level")) urgency_level = j.at("urgency_level").get<double>();
        if (j.contains("time_horizon")) time_horizon = std::chrono::minutes(j.at("time_horizon").get<int>());
        if (j.contains("allow_cross_venue")) allow_cross_venue = j.at("allow_cross_venue").get<bool>();
        if (j.contains("dark_pool_only")) dark_pool_only = j.at("dark_pool_only").get<bool>();
        if (j.contains("max_child_orders")) max_child_orders = j.at("max_child_orders").get<int>();
        if (j.contains("min_child_size")) min_child_size = j.at("min_child_size").get<double>();
        if (j.contains("venues")) venues = j.at("venues").get<std::vector<std::string>>();
        if (j.contains("venue_weights")) venue_weights = j.at("venue_weights").get<std::unordered_map<std::string, double>>();
        if (j.contains("version")) version = j.at("version").get<std::string>();
    }
};

/**
 * @brief Single execution job
 */
struct ExecutionJob {
    std::string job_id;
    std::string parent_order_id;
    ExecutionAlgo algo;
    ExecutionConfig config;
    std::vector<std::string> child_order_ids;
    ExecutionMetrics metrics;
    bool is_complete{false};
    Timestamp start_time;
    Timestamp end_time;
    std::string error_message;
};

/**
 * @brief Engine for order execution and algorithm implementation
 */
class ExecutionEngine {
public:
    explicit ExecutionEngine(std::shared_ptr<OrderManager> order_manager);
    ~ExecutionEngine();

    // Delete copy and move
    ExecutionEngine(const ExecutionEngine&) = delete;
    ExecutionEngine& operator=(const ExecutionEngine&) = delete;
    ExecutionEngine(ExecutionEngine&&) = delete;
    ExecutionEngine& operator=(ExecutionEngine&&) = delete;

    /**
     * @brief Initialize the execution engine
     * @return Result indicating success or failure
     */
    Result<void> initialize();

    /**
     * @brief Submit order for algorithmic execution
     * @param order Parent order to execute
     * @param algo Algorithm to use
     * @param config Execution configuration
     * @return Result containing job ID if successful
     */
    Result<std::string> submit_execution(
        const Order& order,
        ExecutionAlgo algo,
        const ExecutionConfig& config);

    /**
     * @brief Cancel execution job
     * @param job_id ID of job to cancel
     * @return Result indicating success or failure
     */
    Result<void> cancel_execution(const std::string& job_id);

    /**
     * @brief Get execution metrics for a job
     * @param job_id Job ID to query
     * @return Result containing execution metrics
     */
    Result<ExecutionMetrics> get_metrics(const std::string& job_id) const;

    /**
     * @brief Get all active execution jobs
     * @return Result containing vector of active jobs
     */
    Result<std::vector<ExecutionJob>> get_active_jobs() const;

    /**
     * @brief Register custom execution algorithm
     * @param name Algorithm name
     * @param algo Algorithm implementation
     * @return Result indicating success or failure
     */
    Result<void> register_custom_algo(
        const std::string& name,
        std::function<Result<void>(const ExecutionJob&)> algo);

private:
    std::shared_ptr<OrderManager> order_manager_;
    std::unordered_map<std::string, ExecutionJob> active_jobs_;
    std::unordered_map<std::string, std::function<Result<void>(const ExecutionJob&)>> custom_algos_;
    mutable std::mutex mutex_;

    /**
     * @brief Execute market order algorithm
     * @param job Execution job
     * @return Result indicating success or failure
     */
    Result<void> execute_market(const ExecutionJob& job);

    /**
     * @brief Execute TWAP algorithm
     * @param job Execution job
     * @return Result indicating success or failure
     */
    Result<void> execute_twap(const ExecutionJob& job);

    /**
     * @brief Execute VWAP algorithm
     * @param job Execution job
     * @return Result indicating success or failure
     */
    Result<void> execute_vwap(const ExecutionJob& job);

    /**
     * @brief Execute percentage of volume algorithm
     * @param job Execution job
     * @return Result indicating success or failure
     */
    Result<void> execute_pov(const ExecutionJob& job);

    /**
     * @brief Execute adaptive limit order algorithm
     * @param job Execution job
     * @return Result indicating success or failure
     */
    Result<void> execute_adaptive_limit(const ExecutionJob& job);

    /**
     * @brief Execute implementation shortfall algorithm
     * @param job Execution job
     * @return Result indicating success or failure
     */
    Result<void> execute_is(const ExecutionJob& job);

    /**
     * @brief Execute dark pool algorithm
     * @param job Execution job
     * @return Result indicating success or failure
     */
    Result<void> execute_dark_pool(const ExecutionJob& job);

    /**
     * @brief Generate child orders for a job
     * @param job Execution job
     * @param num_slices Number of slices
     * @return Result containing vector of child orders
     */
    Result<std::vector<Order>> generate_child_orders(
        const ExecutionJob& job,
        size_t num_slices);

    /**
     * @brief Update execution metrics
     * @param job_id Job ID to update
     * @param fills Vector of execution reports
     * @return Result indicating success or failure
     */
    Result<void> update_metrics(
        const std::string& job_id,
        const std::vector<ExecutionReport>& fills);

    /**
     * @brief Calculate optimal schedule
     * @param job Execution job
     * @param market_data Market data for scheduling
     * @return Result containing vector of scheduled times and quantities
     */
    Result<std::vector<std::pair<Timestamp, double>>> calculate_schedule(
        const ExecutionJob& job,
        const std::vector<Bar>& market_data);

    /**
     * @brief Generate unique job ID
     * @return Unique job ID
     */
    std::string generate_job_id() const;
};

} // namespace trade_ngin