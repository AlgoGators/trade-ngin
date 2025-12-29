#pragma once

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include <Eigen/Dense>
#include <vector>

namespace trade_ngin {
namespace analysis {

/**
 * @brief Result of HMM fitting
 */
struct HMMFitResult {
    Eigen::MatrixXd transition_matrix;       // State transition probabilities (n_states x n_states)
    Eigen::VectorXd initial_distribution;    // Initial state probabilities
    std::vector<Eigen::VectorXd> emission_means;     // Mean for each state (Gaussian emissions)
    std::vector<Eigen::MatrixXd> emission_covariances; // Covariance for each state
    double log_likelihood;                   // Final log-likelihood
    std::vector<int> state_sequence;         // Most likely state sequence (Viterbi path)
    Eigen::MatrixXd state_probabilities;     // Forward-backward probabilities (n_obs x n_states)
    bool converged;                          // Whether EM algorithm converged
    int n_iterations;                        // Number of EM iterations
};

/**
 * @brief Regime classification for market states
 */
enum class MarketRegime {
    TRENDING_UP,      // Bullish trending market
    TRENDING_DOWN,    // Bearish trending market
    MEAN_REVERTING,   // Range-bound, mean-reverting market
    HIGH_VOLATILITY,  // High volatility regime
    LOW_VOLATILITY    // Low volatility regime
};

/**
 * @brief Hidden Markov Model for regime detection
 *
 * Implements Gaussian HMM with discrete hidden states and continuous observations.
 * Uses Baum-Welch (EM) algorithm for parameter estimation and Viterbi algorithm
 * for state sequence decoding.
 *
 * Model specification:
 *   - Hidden states: S_t ∈ {1, ..., K}
 *   - Observations: O_t ~ N(μ_{S_t}, Σ_{S_t})
 *   - Transition: P(S_t = j | S_{t-1} = i) = A_{ij}
 *   - Initial: P(S_0 = i) = π_i
 *
 * Use cases in trading:
 * - Market regime detection (trending vs mean-reverting)
 * - Volatility regime identification
 * - State-dependent strategy switching
 * - Risk management based on market conditions
 */
class HMM {
public:
    /**
     * @brief Constructor
     * @param n_states Number of hidden states
     * @param n_features Dimensionality of observations
     */
    HMM(int n_states, int n_features);

    /**
     * @brief Fit HMM to observation sequence using Baum-Welch (EM) algorithm
     * @param observations Matrix where each row is an observation (n_obs x n_features)
     * @param max_iterations Maximum EM iterations
     * @param tolerance Convergence tolerance
     * @return Fit result with model parameters
     */
    Result<HMMFitResult> fit(
        const Eigen::MatrixXd& observations,
        int max_iterations = 100,
        double tolerance = 1e-4
    );

    /**
     * @brief Fit HMM to return series
     * @param returns Vector of returns
     * @param max_iterations Maximum EM iterations
     * @param tolerance Convergence tolerance
     * @return Fit result
     */
    Result<HMMFitResult> fit(
        const std::vector<double>& returns,
        int max_iterations = 100,
        double tolerance = 1e-4
    );

    /**
     * @brief Predict most likely state sequence using Viterbi algorithm
     * @param observations Observation sequence
     * @return Most likely state sequence
     */
    Result<std::vector<int>> predict(const Eigen::MatrixXd& observations);

    /**
     * @brief Predict state probabilities using forward-backward algorithm
     * @param observations Observation sequence
     * @return State probability matrix (n_obs x n_states)
     */
    Result<Eigen::MatrixXd> predict_proba(const Eigen::MatrixXd& observations);

    /**
     * @brief Predict current state given new observation
     * @param observation New observation vector
     * @return Most likely current state
     */
    Result<int> predict_state(const Eigen::VectorXd& observation);

    /**
     * @brief Calculate log-likelihood of observation sequence
     * @param observations Observation sequence
     * @return Log-likelihood value
     */
    Result<double> score(const Eigen::MatrixXd& observations);

    /**
     * @brief Get transition probability matrix
     * @return Transition matrix A (n_states x n_states)
     */
    const Eigen::MatrixXd& get_transition_matrix() const { return transition_matrix_; }

    /**
     * @brief Get initial state distribution
     * @return Initial probabilities π (n_states)
     */
    const Eigen::VectorXd& get_initial_distribution() const { return initial_distribution_; }

    /**
     * @brief Get emission parameters for a state
     * @param state State index
     * @return Pair of (mean, covariance)
     */
    std::pair<Eigen::VectorXd, Eigen::MatrixXd> get_emission_params(int state) const;

    /**
     * @brief Check if model has been fitted
     * @return True if fit() has been called successfully
     */
    bool is_fitted() const { return fitted_; }

    /**
     * @brief Get stationary distribution of states
     * @return Stationary probabilities (eigenvector of transition matrix)
     */
    Result<Eigen::VectorXd> get_stationary_distribution() const;

private:
    int n_states_;
    int n_features_;
    bool fitted_;

    // Model parameters
    Eigen::MatrixXd transition_matrix_;      // A: n_states x n_states
    Eigen::VectorXd initial_distribution_;   // π: n_states
    std::vector<Eigen::VectorXd> means_;     // μ_k for each state
    std::vector<Eigen::MatrixXd> covariances_; // Σ_k for each state

    // Current state estimate
    Eigen::VectorXd current_state_probs_;

    // Helper functions
    void initialize_parameters(const Eigen::MatrixXd& observations);

    double gaussian_log_pdf(
        const Eigen::VectorXd& x,
        const Eigen::VectorXd& mean,
        const Eigen::MatrixXd& covariance
    ) const;

    Result<Eigen::MatrixXd> forward_algorithm(const Eigen::MatrixXd& observations);
    Result<Eigen::MatrixXd> backward_algorithm(const Eigen::MatrixXd& observations);
    Result<std::vector<int>> viterbi_algorithm(const Eigen::MatrixXd& observations);

    Result<void> em_step(
        const Eigen::MatrixXd& observations,
        Eigen::MatrixXd& alpha,
        Eigen::MatrixXd& beta,
        Eigen::MatrixXd& gamma,
        std::vector<Eigen::MatrixXd>& xi
    );
};

/**
 * @brief Simple helper: Detect market regimes in price series
 *
 * Fits a 3-state HMM to returns and volatility, identifying:
 * - State 0: Low volatility / mean-reverting
 * - State 1: Medium volatility / trending
 * - State 2: High volatility / choppy
 *
 * @param prices Price series
 * @param window Window size for feature calculation
 * @return HMM fit result with regime classification
 */
Result<HMMFitResult> detect_market_regimes(
    const std::vector<double>& prices,
    int window = 20
);

/**
 * @brief Classify current market regime based on HMM state
 * @param hmm Fitted HMM model
 * @param state Current state from prediction
 * @param observations Recent observations used for classification
 * @return Market regime classification
 */
Result<MarketRegime> classify_regime(
    const HMM& hmm,
    int state,
    const Eigen::MatrixXd& observations
);

} // namespace analysis
} // namespace trade_ngin
