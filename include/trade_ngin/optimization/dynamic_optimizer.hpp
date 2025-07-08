// include/trade_ngin/optimization/dynamic_optimizer.hpp
#pragma once

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/config_base.hpp"
#include "trade_ngin/core/logger.hpp"
#include <vector>
#include <memory>
#include <unordered_map>
#include <nlohmann/json.hpp>

namespace trade_ngin {

/**
 * @brief Configuration for dynamic optimization
 */
struct DynamicOptConfig : public ConfigBase {
    double tau;                       // Risk aversion parameter
    double capital;                    // Trading capital
    double cost_penalty_scalar;        // Multiplier for cost penalty (e.g., 50)
    double asymmetric_risk_buffer;     // Buffer for risk (e.g., 0.1)
    int max_iterations;                // Maximum optimization iterations
    double convergence_threshold;      // Convergence threshold
    bool use_buffering;                // Whether to use position buffering
    double buffer_size_factor;         // Factor for buffer size calculation (e.g., 0.05)

    // Configuration metadata
    std::string version{"1.0.0"};

    // Default constructor with reasonable values
    DynamicOptConfig() 
        : tau(1.0),
          capital(500000.0),
          cost_penalty_scalar(50.0),
          asymmetric_risk_buffer(0.1),
          max_iterations(100),
          convergence_threshold(1e-6),
          use_buffering(true),
          buffer_size_factor(0.05) {}

    nlohmann::json to_json() const override {
        nlohmann::json j;
        j["tau"] = tau;
        j["capital"] = capital;
        j["asymmetric_risk_buffer"] = asymmetric_risk_buffer;
        j["cost_penalty_scalar"] = cost_penalty_scalar;
        j["max_iterations"] = max_iterations;
        j["convergence_threshold"] = convergence_threshold;
        j["use_buffering"] = use_buffering;
        j["buffer_size_factor"] = buffer_size_factor;
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
        if (j.contains("use_buffering")) use_buffering = j.at("use_buffering").get<bool>();
        if (j.contains("buffer_size_factor")) {
            buffer_size_factor = j.at("buffer_size_factor").get<double>();
        }
        if (j.contains("version")) version = j.at("version").get<std::string>();
    }
};

/**
 * @brief Result of dynamic optimization
 */
struct OptimizationResult {
    std::vector<double> positions;     // Optimized positions in weight terms
    double tracking_error;             // Final tracking error
    double cost_penalty;               // Cost penalty component
    int iterations;                    // Number of iterations performed
    bool converged;                    // Whether optimization converged
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
     * @brief Optimize positions for a single trading period with buffering
     * @param current_positions Current positions held
     * @param target_positions Target positions from strategy
     * @param costs Transaction costs per contract
     * @param weights_per_contract Asset weights for risk calculation
     * @param covariance Covariance matrix for risk calculation
     * @return Result containing optimized positions and metrics
     */
    Result<OptimizationResult> optimize(
        const std::vector<double>& current_positions,
        const std::vector<double>& target_positions,
        const std::vector<double>& costs,
        const std::vector<double>& weights_per_contract,
        const std::vector<std::vector<double>>& covariance) const;

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

    /**
     * @brief Apply position buffering to reduce trading costs
     * @param current_positions Current positions held
     * @param optimized_positions Optimized positions from strategy
     * @param target_positions Target positions from strategy
     * @param costs Transaction costs per contract
     * @param weights_per_contract Asset weights for risk calculation
     * @param covariance Covariance matrix for risk calculation
     * @return Result containing optimized positions and metrics
     */
    Result<OptimizationResult> apply_buffering(
        const std::vector<double>& current_positions,
        const std::vector<double>& optimized_positions,
        const std::vector<double>& target_positions,
        const std::vector<double>& costs,
        const std::vector<double>& weights_per_contract,
        const std::vector<std::vector<double>>& covariance) const;

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
     * @brief Calculate pure tracking error (without cost penalty)
     * @param target_positions Target positions
     * @param proposed_positions Proposed positions
     * @param covariance Covariance matrix
     * @return Pure tracking error
     */
    double calculate_pure_tracking_error(
        const std::vector<double>& target_positions,
        const std::vector<double>& proposed_positions,
        const std::vector<std::vector<double>>& covariance) const;

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
     * @brief Convert weights to positions
     * @param weights Asset weights
     * @param weights_per_contract Weights per contract
     * @return Positions calculated from weights
     */
    std::vector<double> weights_to_positions(
        const std::vector<double>& weights,
        const std::vector<double>& weights_per_contract
    ) const;
};

} // namespace trade_ngin