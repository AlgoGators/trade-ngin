// src/optimization/dynamic_optimizer.cpp
#include "trade_ngin/optimization/dynamic_optimizer.hpp"
#include <cmath>
#include <algorithm>
#include <numeric>

namespace trade_ngin {

DynamicOptimizer::DynamicOptimizer(DynamicOptConfig config)
    : config_(std::move(config)) {
    // Initialize logger
    LoggerConfig logger_config;
    logger_config.min_level = LogLevel::DEBUG;
    logger_config.destination = LogDestination::BOTH;
    logger_config.log_directory = "logs";
    logger_config.filename_prefix = "dynamic_optimizer";
    Logger::instance().initialize(logger_config);
    }

Result<void> DynamicOptimizer::validate_inputs(
    const std::vector<double>& current_positions,
    const std::vector<double>& target_positions,
    const std::vector<double>& costs,
    const std::vector<double>& weights,
    const std::vector<std::vector<double>>& covariance) const {
    
    if (current_positions.empty() || target_positions.empty() || costs.empty() || 
        weights.empty() || covariance.empty()) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Input vectors cannot be empty",
            "DynamicOptimizer"
        );
    }

    if (current_positions.size() != target_positions.size() ||
        current_positions.size() != costs.size() ||
        current_positions.size() != weights.size() ||
        current_positions.size() != covariance.size()) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Input vectors must have the same size",
            "DynamicOptimizer"
        );
    }

    for (const auto& row : covariance) {
        if (row.size() != current_positions.size()) {
            return make_error<void>(
                ErrorCode::INVALID_ARGUMENT,
                "Covariance matrix must be square",
                "DynamicOptimizer"
            );
        }
    }

    return Result<void>();
}

Result<OptimizationResult> DynamicOptimizer::optimize_single_period(
    const std::vector<double>& current_positions,
    const std::vector<double>& target_positions,
    const std::vector<double>& costs,
    const std::vector<double>& weights,
    const std::vector<std::vector<double>>& covariance) const {
    
    // Validate inputs
    auto validation = validate_inputs(
        current_positions,
        target_positions,
        costs,
        weights,
        covariance
    );
    
    if (validation.is_error()) {
        return make_error<OptimizationResult>(
            validation.error()->code(),
            validation.error()->what(),
            "DynamicOptimizer"
        );
    }

    try {
        size_t num_assets = current_positions.size();
        std::vector<double> proposed_positions = current_positions;
        std::vector<double> best_positions = current_positions;

        // Calculate initial tracking error
        double initial_cost = calculate_cost_penalty(current_positions, proposed_positions, costs);
        double initial_error = calculate_tracking_error(target_positions, proposed_positions, covariance, initial_cost);

        double best_tracking_error = initial_error;
        double current_tracking_error;
        int iteration = 0;
        bool converged = false;

        // Main optimization loop
        while (iteration++ < config_.max_iterations) {
            bool improved = false;

            // Iterate through each asset
            for (size_t i = 0; i < num_assets; ++i) {
                // Try incremental changes to position
                for (double delta : {-1.0, 1.0}) {
                    std::vector<double> temp_positions = proposed_positions;
                    temp_positions[i] += delta * weights[i];

                    // Calculate cost penalty
                    double cost_penalty = calculate_cost_penalty(
                        current_positions,
                        temp_positions,
                        costs
                    );

                    // Calculate tracking error
                    current_tracking_error = calculate_tracking_error(
                        target_positions,
                        temp_positions,
                        covariance,
                        cost_penalty
                    );

                    // Update if improvement found
                    if (current_tracking_error < best_tracking_error - config_.convergence_threshold) {
                        best_tracking_error = current_tracking_error;
                        best_positions = temp_positions;
                        improved = true;
                    }
                }
            }

            // Check convergence
            if (!improved) {
                converged = true;
                break;
            }

            proposed_positions = best_positions;
        }

        // Round final positions to integers
        auto final_positions = round_to_integer(best_positions);

        // Calculate final metrics
        double final_cost = calculate_cost_penalty(current_positions, final_positions, costs);
        double final_error = calculate_tracking_error(target_positions, final_positions, covariance, final_cost);

        OptimizationResult result{
            final_positions,
            final_error,
            final_cost,
            iteration,
            converged
        };

        return Result<OptimizationResult>(result);

    } catch (const std::exception& e) {
        return make_error<OptimizationResult>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Optimization error: ") + e.what(),
            "DynamicOptimizer"
        );
    }
}

double DynamicOptimizer::calculate_cost_penalty(
    const std::vector<double>& current_positions,
    const std::vector<double>& proposed_positions,
    const std::vector<double>& costs) const {
    
    double penalty = 0.0;
    for (size_t i = 0; i < current_positions.size(); ++i) {
        double trade_size = std::abs(proposed_positions[i] - current_positions[i]);
        penalty += trade_size * costs[i] * config_.cost_penalty_scalar;
        
        // Add asymmetric risk buffer for large position changes
        if (trade_size > 0) {
            penalty += config_.asymmetric_risk_buffer * trade_size * costs[i];
        }
    }
    return penalty;
}

double DynamicOptimizer::calculate_tracking_error(
    const std::vector<double>& target_positions,
    const std::vector<double>& proposed_positions,
    const std::vector<std::vector<double>>& covariance,
    double cost_penalty) const {
    
    double tracking_error = 0.0;
    size_t n = target_positions.size();

    // Calculate position differences
    std::vector<double> position_diff(n);
    for (size_t i = 0; i < n; ++i) {
        position_diff[i] = proposed_positions[i] - target_positions[i];
    }

    // Calculate quadratic tracking error
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < n; ++j) {
            tracking_error += position_diff[i] * covariance[i][j] * position_diff[j];
        }
    }

    // Scale by risk aversion and add cost penalty
    return config_.tau * std::sqrt(std::max(0.0, tracking_error)) + cost_penalty;
}

std::vector<double> DynamicOptimizer::round_to_integer(
    const std::vector<double>& positions) const {
    
    std::vector<double> rounded_positions(positions.size());
    std::transform(positions.begin(), positions.end(), rounded_positions.begin(),
        [](double pos) { return std::round(pos); });
    return rounded_positions;
}

Result<void> DynamicOptimizer::update_config(const DynamicOptConfig& config) {
    if (config.tau <= 0.0) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Risk aversion parameter must be positive",
            "DynamicOptimizer"
        );
    }

    if (config.capital <= 0.0) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Capital must be positive",
            "DynamicOptimizer"
        );
    }

    if (config.cost_penalty_scalar <= 0) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Cost penalty scalar must be positive",
            "DynamicOptimizer"
        );
    }

    config_ = config;
    return Result<void>();
}

} // namespace trade_ngin