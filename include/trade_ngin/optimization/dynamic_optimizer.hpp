// include/trade_ngin/optimization/dynamic_optimizer.hpp
#pragma once

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/config_base.hpp"
#include "trade_ngin/core/logger.hpp"
#include <vector>
#include <memory>
#include <unordered_map>

namespace trade_ngin {

/**
 * @brief Configuration for dynamic optimization
 */
struct DynamicOptConfig : public ConfigBase {
    double tau{1.0};                    // Risk aversion parameter
    double capital{0.0};                // Total capital
    double asymmetric_risk_buffer{0.1}; // Risk buffer for asymmetric costs
    int cost_penalty_scalar{10};        // Scalar for trading cost penalty
    size_t max_iterations{1000};        // Maximum optimization iterations
    double convergence_threshold{1e-6}; // Convergence threshold

    // Configuration metadata
    std::string version{"1.0.0"};

    DynamicOptConfig() = default;

    DynamicOptConfig(double tau, double capital, double asymmetric_risk_buffer, 
        int cost_penalty_scalar, size_t max_iterations, double convergence_threshold)
        : tau(tau), capital(capital), asymmetric_risk_buffer(asymmetric_risk_buffer),
          cost_penalty_scalar(cost_penalty_scalar), max_iterations(max_iterations),
          convergence_threshold(convergence_threshold) {}

    nlohmann::json to_json() const override {
        nlohmann::json j;
        j["tau"] = tau;
        j["capital"] = capital;
        j["asymmetric_risk_buffer"] = asymmetric_risk_buffer;
        j["cost_penalty_scalar"] = cost_penalty_scalar;
        j["max_iterations"] = max_iterations;
        j["convergence_threshold"] = convergence_threshold;
        j["version"] = version;
        return j;
    }

    void from_json(const nlohmann::json& j) override {
        if (j.contains("tau")) tau = j.at("tau").get<double>();
        if (j.contains("capital")) capital = j.at("capital").get<double>();
        if (j.contains("asymmetric_risk_buffer")) {
            asymmetric_risk_buffer = j.at("asymmetric_risk_buffer").get<double>();
        }
        if (j.contains("cost_penalty_scalar")) {
            cost_penalty_scalar = j.at("cost_penalty_scalar").get<int>();
        }
        if (j.contains("max_iterations")) {
            max_iterations = j.at("max_iterations").get<size_t>();
        }
        if (j.contains("convergence_threshold")) {
            convergence_threshold = j.at("convergence_threshold").get<double>();
        }
        if (j.contains("version")) version = j.at("version").get<std::string>();
    }
};

/**
 * @brief Result of dynamic optimization
 */
struct OptimizationResult {
    std::vector<double> optimized_positions;  // Optimized positions
    double tracking_error;                    // Final tracking error
    double trading_cost;                      // Total trading cost
    int iterations;                           // Number of iterations used
    bool converged;                          // Whether optimization converged
};

/**
 * @brief Dynamic position optimizer
 * 
 * Optimizes trading positions considering transaction costs and tracking error
 */
class DynamicOptimizer {
public:
    /**
     * @brief Constructor
     * @param config Optimization configuration
     */
    explicit DynamicOptimizer(DynamicOptConfig config);

    /**
     * @brief Optimize positions for a single trading period
     * @param current_positions Current positions held
     * @param target_positions Target positions from strategy
     * @param costs Transaction costs per contract
     * @param weights Asset weights for risk calculation
     * @param covariance Covariance matrix for risk calculation
     * @return Result containing optimized positions and metrics
     */
    Result<OptimizationResult> optimize_single_period(
        const std::vector<double>& current_positions,
        const std::vector<double>& target_positions,
        const std::vector<double>& costs,
        const std::vector<double>& weights,
        const std::vector<std::vector<double>>& covariance
    ) const;

    /**
     * @brief Update configuration
     * @param config New configuration
     * @return Result indicating success or failure
     */
    Result<void> update_config(const DynamicOptConfig& config);

    /**
     * @brief Get current configuration
     * @return Current configuration
     */
    const DynamicOptConfig& get_config() const { return config_; }

private:
    DynamicOptConfig config_;

    /**
     * @brief Calculate trading cost penalty
     * @param current_positions Current positions
     * @param proposed_positions Proposed new positions
     * @param costs Cost per contract
     * @return Trading cost penalty
     */
    double calculate_cost_penalty(
        const std::vector<double>& current_positions,
        const std::vector<double>& proposed_positions,
        const std::vector<double>& costs
    ) const;

    /**
     * @brief Calculate tracking error
     * @param target_positions Target positions
     * @param proposed_positions Proposed positions
     * @param covariance Covariance matrix
     * @param cost_penalty Trading cost penalty
     * @return Total tracking error including cost penalty
     */
    double calculate_tracking_error(
        const std::vector<double>& target_positions,
        const std::vector<double>& proposed_positions,
        const std::vector<std::vector<double>>& covariance,
        double cost_penalty
    ) const;

    /**
     * @brief Round positions to integer values
     * @param positions Raw positions
     * @return Rounded positions
     */
    std::vector<double> round_to_integer(const std::vector<double>& positions) const;

    /**
     * @brief Validate inputs for optimization
     * @return Result indicating if inputs are valid
     */
    Result<void> validate_inputs(
        const std::vector<double>& current_positions,
        const std::vector<double>& target_positions,
        const std::vector<double>& costs,
        const std::vector<double>& weights,
        const std::vector<std::vector<double>>& covariance
    ) const;
};

} // namespace trade_ngin