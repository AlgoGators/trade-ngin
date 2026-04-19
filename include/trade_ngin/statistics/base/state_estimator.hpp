#pragma once
#define TRADE_NGIN_STATISTICS_BASE_STATE_ESTIMATOR_HPP

#include "trade_ngin/core/error.hpp"
#include <Eigen/Dense>

namespace trade_ngin {
namespace statistics {

/**
 * @brief Base class for state estimation models
 */
class StateEstimator {
public:
    virtual ~StateEstimator() = default;

    /**
     * @brief Initialize the state estimator
     * @param initial_state Initial state vector
     * @return Result indicating success or failure
     */
    virtual Result<void> initialize(const Eigen::VectorXd& initial_state) = 0;

    /**
     * @brief Predict next state
     * @return Predicted state vector
     */
    virtual Result<Eigen::VectorXd> predict() = 0;

    /**
     * @brief Update state with new observation
     * @param observation New observation vector
     * @return Updated state vector
     */
    virtual Result<Eigen::VectorXd> update(const Eigen::VectorXd& observation) = 0;

    /**
     * @brief Get current state estimate
     */
    virtual Result<Eigen::VectorXd> get_state() const = 0;

    /**
     * @brief Check if estimator is initialized
     */
    virtual bool is_initialized() const = 0;
};

} // namespace statistics
} // namespace trade_ngin
