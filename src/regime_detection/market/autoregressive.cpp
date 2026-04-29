#include "trade_ngin/regime_detection/market/msar.hpp"
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


Result<void> MarketMSAR::fit(const Eigen::VectorXd& returns,
                             const Eigen::MatrixXd& state_probs,
                             const Eigen::MatrixXd& transition_matrix) {
    if (returns.size() <= lag_) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "MarketMSAR::fit requires returns.size() > lag.",
                                "MarketMSAR");
    }
    if (state_probs.rows() != returns.size()) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "state_probs rows must equal returns size.",
                                "MarketMSAR");
    }
    if (transition_matrix.rows() != transition_matrix.cols()) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "transition_matrix must be square.",
                                "MarketMSAR");
    }
    if (state_probs.cols() != transition_matrix.rows()) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "state_probs cols must match transition_matrix size.",
                                "MarketMSAR");
    }


    n_states_ = static_cast<int>(state_probs.cols());

    // Re-estimate transition_matrix_ from the smoothed posteriors
    // before AR fitting. The pre-fit transition matrix passed in is from
    // MarkovSwitching fit on RAW returns; the AR step then uses these
    // posteriors to estimate AR coeffs in regime-conditional space, but
    // the stored transition matrix would still reflect the raw-return
    // dynamics — inconsistent. We recompute using the standard smoothed-
    // posterior approximation:
    //   P(i→j) ≈ Σ_t γ(t,i) γ(t+1,j) / Σ_t γ(t,i)
    // This is an approximation (proper Baum-Welch xi requires forward-
    // backward), but it guarantees the stored transition is internally
    // consistent with the state_probs used for AR fitting.
    {
        const Eigen::Index Tp = state_probs.rows();
        Eigen::MatrixXd P_recomputed = Eigen::MatrixXd::Zero(n_states_, n_states_);
        for (int i = 0; i < n_states_; ++i) {
            double row_sum = 0.0;
            for (Eigen::Index t = 0; t < Tp - 1; ++t) {
                row_sum += state_probs(t, i);
                for (int j = 0; j < n_states_; ++j) {
                    P_recomputed(i, j) += state_probs(t, i) * state_probs(t + 1, j);
                }
            }
            if (row_sum > 1e-10) {
                for (int j = 0; j < n_states_; ++j) P_recomputed(i, j) /= row_sum;
            } else {
                // Fallback to passed-in matrix row if posterior is empty for this state
                for (int j = 0; j < n_states_; ++j) P_recomputed(i, j) = transition_matrix(i, j);
            }
        }
        transition_matrix_ = P_recomputed;
    }

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


    return Result<void>();
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


Result<MSARForecastBreakdown> MarketMSAR::predict_next_detailed(const Eigen::VectorXd& lag_window,
                                                                 const Eigen::VectorXd& current_probs) const {
    if (lag_window.size() != lag_) {
        return make_error<MSARForecastBreakdown>(ErrorCode::INVALID_ARGUMENT,
                                                 "lag_window size must equal lag.",
                                                 "MarketMSAR");
    }
    if (current_probs.size() != n_states_) {
        return make_error<MSARForecastBreakdown>(ErrorCode::INVALID_ARGUMENT,
                                                 "current_probs size must equal n_states.",
                                                 "MarketMSAR");
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


Result<double> MarketMSAR::predict_next(const Eigen::VectorXd& lag_window,
                                        const Eigen::VectorXd& current_probs) const {
    auto result = predict_next_detailed(lag_window, current_probs);
    if (result.is_error()) {
        return make_error<double>(result.error()->code(),
                                  result.error()->what(),
                                  result.error()->component());
    }
    return result.value().weighted_forecast;
}


// -----------------------------------------------------------------------------
// Historical backtest helper
// -----------------------------------------------------------------------------


Result<MSARBacktestResult> historical_backtest_market_msar(
    const Eigen::VectorXd& returns,
    int ar_lag,
    std::size_t min_train_size,
    const MarkovSwitchingConfig& ms_config,
    bool verbose
) {
    if (ar_lag <= 0) {
        return make_error<MSARBacktestResult>(ErrorCode::INVALID_ARGUMENT,
                                              "ar_lag must be positive.",
                                              "historical_backtest_market_msar");
    }
    if (returns.size() <= static_cast<Eigen::Index>(min_train_size)) {
        return make_error<MSARBacktestResult>(ErrorCode::INVALID_ARGUMENT,
                                              "Not enough data for historical backtest.",
                                              "historical_backtest_market_msar");
    }
    if (min_train_size <= static_cast<std::size_t>(ar_lag)) {
        return make_error<MSARBacktestResult>(ErrorCode::INVALID_ARGUMENT,
                                              "min_train_size must be > ar_lag.",
                                              "historical_backtest_market_msar");
    }


    MSARBacktestResult result;
    double abs_error_sum = 0.0;
    double sq_error_sum = 0.0;


    for (Eigen::Index t {static_cast<Eigen::Index>(min_train_size)}; t < returns.size(); ++t) {
        Eigen::VectorXd train_returns = returns.head(t);


        MarkovSwitching ms_model(ms_config);
        auto fit_result = ms_model.fit(eigen_to_std_vector(train_returns));
        if (fit_result.is_error()) {
            return make_error<MSARBacktestResult>(fit_result.error()->code(),
                                                  fit_result.error()->what(),
                                                  "historical_backtest_market_msar");
        }
        const MarkovSwitchingResult& ms = fit_result.value();


        MarketMSAR msar(ar_lag);
        auto msar_fit = msar.fit(train_returns, ms.smoothed_probabilities, ms.transition_matrix);
        if (msar_fit.is_error()) {
            return make_error<MSARBacktestResult>(msar_fit.error()->code(),
                                                  msar_fit.error()->what(),
                                                  "historical_backtest_market_msar");
        }


        Eigen::VectorXd lag_window = returns.segment(t - ar_lag, ar_lag);
        Eigen::VectorXd current_probs = ms.smoothed_probabilities.row(train_returns.size() - 1).transpose();


        auto pred_result = msar.predict_next(lag_window, current_probs);
        if (pred_result.is_error()) {
            return make_error<MSARBacktestResult>(pred_result.error()->code(),
                                                  pred_result.error()->what(),
                                                  "historical_backtest_market_msar");
        }
        double prediction = pred_result.value();
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