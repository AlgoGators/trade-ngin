//#include "trade_ngin/statistics/state_estimation/msar.hpp"
#include "trade_ngin/src/models/autoregression/msar.hpp"
#include "trade_ngin/statistics/state_estimation/markov_switching.hpp"


#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>


namespace trade_ngin {
namespace statistics {


namespace {


std::vector<double> eigen_to_std_vector(const Eigen::VectorXd& values) {
    std::vector<double> out(static_cast<std::size_t>(values.size()));
    for (Eigen::Index i = 0; i < values.size(); ++i) {
        out[static_cast<std::size_t>(i)] = values(i);
    }
    return out;
}


}


MarketMSAR::MarketMSAR(int lag)
    : lag_(lag) {
    if (lag_ <= 0) {
        throw std::invalid_argument("MarketMSAR lag must be positive.");
    }
}


void MarketMSAR::fit(const Eigen::VectorXd& returns,
                     const Eigen::MatrixXd& state_probs,
                     const Eigen::MatrixXd& transition_matrix) {
    if (returns.size() <= lag_) {
        throw std::invalid_argument("MarketMSAR::fit requires returns.size() > lag.");
    }
    if (state_probs.rows() != returns.size()) {
        throw std::invalid_argument("state_probs rows must equal returns size.");
    }
    if (transition_matrix.rows() != transition_matrix.cols()) {
        throw std::invalid_argument("transition_matrix must be square.");
    }
    if (state_probs.cols() != transition_matrix.rows()) {
        throw std::invalid_argument("state_probs cols must match transition_matrix size.");
    }


    n_states_ = static_cast<int>(state_probs.cols());
    transition_matrix_ = transition_matrix;
    ar_coeffs_ = Eigen::MatrixXd::Zero(n_states_, lag_);
    intercepts_ = Eigen::VectorXd::Zero(n_states_);
    residual_variances_ = Eigen::VectorXd::Zero(n_states_);


    const Eigen::Index T = returns.size();
    const Eigen::Index N = T - lag_;


    Eigen::MatrixXd X(N, lag_);
    Eigen::VectorXd y(N);


    // Fill X (lagged return data) and y (current/target returns)
    for (Eigen::Index t = lag_; t < T; ++t) {
        const Eigen::Index row = t - lag_;
        y(row) = returns(t);


        for (size_t k {0}; k < lag_; ++k) {
            X(row, k) = returns(t - 1 - k);
        }
    }


    for (int s {0}; s < n_states_; ++s) {
        Eigen::VectorXd weights(N);
        for (Eigen::Index t = lag_; t < T; ++t) {
            weights(t - lag_) = std::max(state_probs(t, s), 1e-8);
        }


        Eigen::VectorXd coeffs(lag_);
        double intercept = 0.0;
        double residual_var = 0.0;


        fit_single_regime(X, y, weights, coeffs, intercept, residual_var);


        ar_coeffs_.row(s) = coeffs.transpose();
        intercepts_(s) = intercept;
        residual_variances_(s) = residual_var;
    }
}


void MarketMSAR::fit_single_regime(const Eigen::MatrixXd& X,
                                   const Eigen::VectorXd& y,
                                   const Eigen::VectorXd& weights,
                                   Eigen::VectorXd& coeffs,
                                   double& intercept,
                                   double& residual_var) {
    const Eigen::Index N = X.rows();
    const Eigen::Index lag_order = X.cols();


    Eigen::MatrixXd X_aug(N, lag_order + 1); // additional column for intercept
    X_aug.col(0) = Eigen::VectorXd::Ones(N);
    X_aug.block(0, 1, N, lag_order) = X;


    Eigen::VectorXd w = weights.array().max(1e-8);
    Eigen::MatrixXd W = w.asDiagonal();


    Eigen::MatrixXd normal_mat = X_aug.transpose() * W * X_aug;
    Eigen::VectorXd rhs = X_aug.transpose() * W * y;


    Eigen::VectorXd beta = normal_mat.ldlt().solve(rhs);


    intercept = beta(0);
    coeffs = beta.tail(lag_order);


    Eigen::VectorXd residuals = y - X_aug * beta;
    double w_sum = w.sum();
    residual_var = (w_sum > 0.0)
        ? (residuals.array().square() * w.array()).sum() / w_sum
        : 0.0;
}


MSARForecastBreakdown MarketMSAR::predict_next_detailed(const Eigen::VectorXd& lag_window,
                                                        const Eigen::VectorXd& current_probs) const {
    if (lag_window.size() != lag_) {
        throw std::invalid_argument("lag_window size must equal lag.");
    }
    if (current_probs.size() != n_states_) {
        throw std::invalid_argument("current_probs size must equal n_states.");
    }


    MSARForecastBreakdown out;
    out.next_state_probs = transition_matrix_.transpose() * current_probs;
    out.regime_predictions = Eigen::VectorXd::Zero(n_states_);


    for (int s = 0; s < n_states_; ++s) {
        double pred = intercepts_(s);
        for (int k = 0; k < lag_; ++k) {
            pred += ar_coeffs_(s, k) * lag_window(lag_window.size() - 1 - k);
        }
        out.regime_predictions(s) = pred;
        out.weighted_forecast += out.next_state_probs(s) * pred;
    }


    return out;
}


double MarketMSAR::predict_next(const Eigen::VectorXd& lag_window,
                                const Eigen::VectorXd& current_probs) const {
    return predict_next_detailed(lag_window, current_probs).weighted_forecast;
}


// -----------------------------------------------------------------------------
// Historical backtest helper
// -----------------------------------------------------------------------------


MSARBacktestResult historical_backtest_market_msar(
    const Eigen::VectorXd& returns,
    int ar_lag,
    std::size_t min_train_size,
    const MarkovSwitchingConfig& ms_config,
    bool verbose = true
) {
    if (ar_lag <= 0) {
        throw std::invalid_argument("ar_lag must be positive.");
    }
    if (returns.size() <= static_cast<Eigen::Index>(min_train_size)) {
        throw std::invalid_argument("Not enough data for historical backtest.");
    }
    if (min_train_size <= static_cast<std::size_t>(ar_lag)) {
        throw std::invalid_argument("min_train_size must be > ar_lag.");
    }


    MSARBacktestResult result;
    double abs_error_sum = 0.0;
    double sq_error_sum = 0.0;


    for (Eigen::Index t {static_cast<Eigen::Index>(min_train_size)}; t < returns.size(); ++t) {
        Eigen::VectorXd train_returns = returns.head(t);


        MarkovSwitching ms_model(ms_config);
        auto fit_result = ms_model.fit(eigen_to_std_vector(train_returns));
        const MarkovSwitchingResult& ms = fit_result.value();


        MarketMSAR msar(ar_lag);
        msar.fit(train_returns, ms.smoothed_probabilities, ms.transition_matrix);


        Eigen::VectorXd lag_window = returns.segment(t - ar_lag, ar_lag);
        Eigen::VectorXd current_probs = ms.smoothed_probabilities.row(train_returns.size() - 1).transpose();


        double prediction = msar.predict_next(lag_window, current_probs);
        double actual = returns(t);
        double abs_error = std::abs(prediction - actual);
        double sq_error = (prediction - actual) * (prediction - actual);


        result.points.push_back(MSARBacktestPoint{
            static_cast<std::size_t>(t),
            static_cast<std::size_t>(train_returns.size()),
            prediction,
            actual,
            abs_error,
            sq_error
        });


        abs_error_sum += abs_error;
        sq_error_sum += sq_error;


        if (verbose) {
            std::cout
                << "Step " << result.points.size()
                << "  t=" << t
                << "  train_size=" << train_returns.size()
                << "  pred=" << prediction
                << "  actual=" << actual
                << "  abs_err=" << abs_error
                << "  sq_err=" << sq_error
                << '\n';
        }
    }


    if (!result.points.empty()) {
        result.mae = abs_error_sum / static_cast<double>(result.points.size());
        result.rmse = std::sqrt(sq_error_sum / static_cast<double>(result.points.size()));
    }


    return result;
}


} // namespace statistics
} // namespace trade_ngin