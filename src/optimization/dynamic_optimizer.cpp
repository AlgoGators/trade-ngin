// src/optimization/dynamic_optimizer.cpp
#include "trade_ngin/optimization/dynamic_optimizer.hpp"
#include <cmath>
#include <algorithm>
#include <numeric>

namespace trade_ngin {

DynamicOptimizer::DynamicOptimizer(DynamicOptConfig config)
    : config_(std::move(config)) {
        Logger::register_component("DynamicOptimizer");
    }

Result<void> DynamicOptimizer::validate_inputs(
    const std::vector<double>& current_positions,
    const std::vector<double>& target_positions,
    const std::vector<double>& costs,
    const std::vector<double>& weights_per_contract,
    const std::vector<std::vector<double>>& covariance) const {
    
    if (current_positions.empty() || target_positions.empty() || costs.empty() || 
        weights_per_contract.empty() || covariance.empty()) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Input vectors cannot be empty",
            "DynamicOptimizer"
        );
    }

    if (current_positions.size() != target_positions.size() ||
        current_positions.size() != costs.size() ||
        current_positions.size() != weights_per_contract.size() ||
        current_positions.size() != covariance.size()) {
        std::string size_error = "Input vectors must have the same size: "
            + std::to_string(current_positions.size()) + ", "
            + std::to_string(target_positions.size()) + ", "
            + std::to_string(costs.size()) + ", "
            + std::to_string(weights_per_contract.size()) + ", "
            + std::to_string(covariance.size());
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            size_error,
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

Result<OptimizationResult> DynamicOptimizer::optimize(
    const std::vector<double>& current_positions,
    const std::vector<double>& target_positions,
    const std::vector<double>& costs,
    const std::vector<double>& weights_per_contract,
    const std::vector<std::vector<double>>& covariance) const {
    
    // First perform standard optimization
    auto result = optimize_single_period(
        current_positions, target_positions, costs, weights_per_contract, covariance);
    
    // Apply buffering if enabled
    if (config_.use_buffering && result.is_ok()) {
        result = apply_buffering(
            current_positions, 
            result.value().positions,
            target_positions,
            costs,
            weights_per_contract,
            covariance);
    }
    
    return result;
}

Result<OptimizationResult> DynamicOptimizer::optimize_single_period(
    const std::vector<double>& current_positions,
    const std::vector<double>& target_positions,
    const std::vector<double>& costs,
    const std::vector<double>& weights_per_contract,
    const std::vector<std::vector<double>>& covariance) const {
    
    // Validate inputs
    auto validation = validate_inputs(
        current_positions,
        target_positions,
        costs,
        weights_per_contract,
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
        // Initialize solution at 0.0

        const auto actual = current_positions;

        size_t num_assets = current_positions.size();
        std::vector<double> current_best(target_positions.size(), 0.0);
        std::vector<double> proposed_solution = current_best;

        for (size_t i = 0; i < num_assets; ++i) {
            DEBUG("DynOpt INIT [" + std::to_string(i) + "] current=" 
                  + std::to_string(current_positions[i]) 
                  + ", target=" + std::to_string(target_positions[i]));
        }
        
        // Calculate initial tracking error
        double cost_penalty = calculate_cost_penalty(actual, current_best, costs);
        double best_tracking_error = calculate_tracking_error(
            target_positions, current_best, covariance, cost_penalty);
        
        bool improved = true;
        int iteration = 0;
        
        // Main optimization loop - using greedy algorithm
        while (improved && iteration++ < config_.max_iterations) {
            improved = false;

            std::vector<double> proposed = current_best;
            double proposed_err = best_tracking_error;
            
            for (size_t i = 0; i < num_assets; ++i) {
                // Try adding one weight unit (one contract)
                double raw_diff = target_positions[i] - current_best[i];
                if (std::abs(raw_diff) < 1e-12) {
                    continue;  // No need to adjust if already close to target
                }

                double step = weights_per_contract[i] * (raw_diff > 0 ? +1.0 : -1.0);

                std::vector<double> candidate = current_best;
                candidate[i] += step;

            // Evaluate its cost & error
            double c = calculate_cost_penalty(actual, candidate, costs);
            double e = calculate_tracking_error(target_positions, candidate, covariance, c);

            // If it strictly improves this pass’s proposed, keep it
            if (e + config_.convergence_threshold < proposed_err) {
                proposed = std::move(candidate);
                proposed_err = e;
            }
        }

        // 4) If the best candidate from this pass is better than our overall best, adopt it
        if (proposed_err + config_.convergence_threshold < best_tracking_error) {
            current_best = std::move(proposed);
            best_tracking_error = proposed_err;
            improved = true;
        }
    }

    // Final metrics
    double final_cost = calculate_cost_penalty(actual, current_best, costs);
    double final_err  = calculate_tracking_error(target_positions, current_best, covariance, final_cost);

    OptimizationResult result {
        current_best,
        final_err,
        final_cost,
        iteration,
        !improved
    };

    DEBUG("Final positions: " + std::to_string(current_best.size()) +
          ", tracking error: " + std::to_string(final_err) +
          ", cost: " + std::to_string(final_cost) +
          ", iterations: " + std::to_string(iteration));

    return Result<OptimizationResult>(result);
        
    } catch (const std::exception& e) {
        return make_error<OptimizationResult>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Optimization error: ") + e.what(),
            "DynamicOptimizer"
        );
    }
}

Result<OptimizationResult> DynamicOptimizer::apply_buffering(
    const std::vector<double>& current_positions,
    const std::vector<double>& optimized_positions,
    const std::vector<double>& target_positions,
    const std::vector<double>& costs,
    const std::vector<double>& weights_per_contract,
    const std::vector<std::vector<double>>& covariance) const {
    
    try {
        // Calculate buffer size based on risk target
        double buffer_size = config_.buffer_size_factor * config_.tau;
        
        // Calculate tracking error between current and optimized positions (pure, without costs)
        double tracking_error = calculate_pure_tracking_error(
            optimized_positions, current_positions, covariance);
        
        // If tracking error is less than buffer, no trading needed
        if (tracking_error <= buffer_size) {
            DEBUG("Tracking error " + std::to_string(tracking_error) + 
                  " is below buffer " + std::to_string(buffer_size) + 
                  ". No trading required.");
            
            // Return current positions with updated metrics
            double current_cost = calculate_cost_penalty(current_positions, current_positions, costs);
            double current_error = calculate_tracking_error(
                target_positions, current_positions, covariance, current_cost);
            
            OptimizationResult result{
                current_positions,
                current_error,
                current_cost,
                0,
                true
            };
            
            return Result<OptimizationResult>(result);
        }
        
        // Calculate adjustment factor to trade to edge of buffer
        double adjustment_factor = std::max((tracking_error - buffer_size) / tracking_error, 0.0);
        
        DEBUG("Tracking error " + std::to_string(tracking_error) + 
              " exceeds buffer " + std::to_string(buffer_size) + 
              ". Adjustment factor: " + std::to_string(adjustment_factor));
        
        // Calculate required trades with buffering
        std::vector<double> buffered_positions = current_positions;
        for (size_t i = 0; i < current_positions.size(); ++i) {
            double required_trade = (optimized_positions[i] - current_positions[i]) * adjustment_factor;
            buffered_positions[i] = current_positions[i] + required_trade;
        }
        
        // Convert to contract units and round to integers
        std::vector<double> final_positions(buffered_positions.size());
        for (size_t i = 0; i < buffered_positions.size(); ++i) {
            if (weights_per_contract[i] > 0) {
                // Calculate number of contracts and round to integer
                double contracts = buffered_positions[i] / weights_per_contract[i];
                int rounded_contracts = std::round(contracts);
                
                // Convert back to weight terms
                final_positions[i] = rounded_contracts * weights_per_contract[i];
            } else {
                final_positions[i] = 0.0;  // Avoid division by zero
            }
        }
        
        // Calculate final metrics
        double final_cost = calculate_cost_penalty(current_positions, final_positions, costs);
        double final_error = calculate_tracking_error(
            target_positions, final_positions, covariance, final_cost);
        
        OptimizationResult result{
            final_positions,
            final_error,
            final_cost,
            0,  // No iterations for buffering
            true
        };
        
        return Result<OptimizationResult>(result);
        
    } catch (const std::exception& e) {
        return make_error<OptimizationResult>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Buffering error: ") + e.what(),
            "DynamicOptimizer"
        );
    }
}

double DynamicOptimizer::calculate_cost_penalty(
    const std::vector<double>& current_positions,
    const std::vector<double>& proposed_positions,
    const std::vector<double>& costs) const {
    
    double total_cost = 0.0;
    
    // Calculate cost of all trades
    for (size_t i = 0; i < current_positions.size(); ++i) {
        // Calculate trade size in weight terms
        double trade_size = std::abs(proposed_positions[i] - current_positions[i]);
        
        // Calculate cost in weight terms
        total_cost += trade_size * costs[i];
    }
    
    // Apply cost penalty scalar (50 in the example)
    return total_cost * config_.cost_penalty_scalar;
}

double DynamicOptimizer::calculate_pure_tracking_error(
    const std::vector<double>& target_positions,
    const std::vector<double>& proposed_positions,
    const std::vector<std::vector<double>>& covariance) const {
    
    // Calculate tracking error weights (e = w* - w)
    std::vector<double> tracking_error_weights(target_positions.size());
    double max_diff = 0.0;
    for (size_t i = 0; i < target_positions.size(); ++i) {
        tracking_error_weights[i] = target_positions[i] - proposed_positions[i];
        max_diff = std::max(max_diff, std::abs(tracking_error_weights[i]));
    }

    // Log covariance matrix values
    double max_cov = 0.0;
    double sum_cov = 0.0;
    for (size_t i = 0; i < covariance.size(); ++i) {
        for (size_t j = 0; j < covariance[i].size(); ++j) {
            max_cov = std::max(max_cov, std::abs(covariance[i][j]));
            sum_cov += std::abs(covariance[i][j]);
        }
    }

    
    // Calculate quadratic form e'Σe
    double tracking_error_squared = 0.0;
    for (size_t i = 0; i < tracking_error_weights.size(); ++i) {
        for (size_t j = 0; j < tracking_error_weights.size(); ++j) {
            tracking_error_squared += tracking_error_weights[i] * 
                                    covariance[i][j] * 
                                    tracking_error_weights[j];
        }
    }
    
    double result = std::sqrt(std::max(0.0, tracking_error_squared));
    
    // Return standard deviation (square root of variance)
    return result;
}

double DynamicOptimizer::calculate_tracking_error(
    const std::vector<double>& target_positions,
    const std::vector<double>& proposed_positions,
    const std::vector<std::vector<double>>& covariance,
    double cost_penalty) const {
    
    // Calculate pure tracking error (standard deviation component)
    double std_dev = calculate_pure_tracking_error(
        target_positions, proposed_positions, covariance);
    
    // Add cost penalty to get total tracking error
    return std_dev + cost_penalty;
}

Result<void> DynamicOptimizer::update_config(const DynamicOptConfig& config) {
    // Validate configuration parameters
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

    if (config.cost_penalty_scalar < 0.0) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Cost penalty scalar must be non-negative",
            "DynamicOptimizer"
        );
    }

    if (config.buffer_size_factor < 0.0) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Buffer size factor must be non-negative",
            "DynamicOptimizer"
        );
    }

    config_ = config;
    return Result<void>();
}

} // namespace trade_ngin