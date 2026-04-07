#pragma once


#include "trade_ngin/statistics/state_estimation/markov_switching.hpp"
#include "trade_ngin/core/error.hpp"
#include <Eigen/Dense>
#include <vector>
#include <cstddef>


namespace trade_ngin {
namespace statistics {


struct MSARForecastBreakdown {
    Eigen::VectorXd next_state_probs;
    Eigen::VectorXd regime_predictions;
    double weighted_forecast{0.0};
};


struct MSARBacktestPoint {
    std::size_t t{0};
    std::size_t train_size{0};
    double prediction{0.0};
    double actual{0.0};
    double abs_error{0.0};
    double sq_error{0.0};
};


struct MSARBacktestResult {
    std::vector<MSARBacktestPoint> points;
    double mae{0.0};
    double rmse{0.0};
};


class MarketMSAR {
public:
    explicit MarketMSAR(int lag);


    Result<void> fit(const Eigen::VectorXd& returns,
                      const Eigen::MatrixXd& state_probs,
                      const Eigen::MatrixXd& transition_matrix);


    Result<double> predict_next(const Eigen::VectorXd& lag_window,
                               const Eigen::VectorXd& current_probs) const;


    Result<MSARForecastBreakdown> predict_next_detailed(const Eigen::VectorXd& lag_window,
                                                        const Eigen::VectorXd& current_probs) const;


    const Eigen::MatrixXd& get_ar_coeffs() const { return ar_coeffs_; }
    const Eigen::VectorXd& get_intercepts() const { return intercepts_; }
    const Eigen::VectorXd& get_residual_variances() const { return residual_variances_; }
    const Eigen::MatrixXd& get_transition_matrix() const { return transition_matrix_; }


private:
    int lag_{1};
    int n_states_{0};


    Eigen::MatrixXd ar_coeffs_;
    Eigen::VectorXd intercepts_;
    Eigen::VectorXd residual_variances_;
    Eigen::MatrixXd transition_matrix_;


    void fit_single_regime(const Eigen::MatrixXd& X,
                           const Eigen::VectorXd& y,
                           const Eigen::VectorXd& weights,
                           Eigen::VectorXd& coeffs,
                           double& intercept,
                           double& residual_var);
};


Result<MSARBacktestResult> historical_backtest_market_msar(
    const Eigen::VectorXd& returns,
    int ar_lag,
    std::size_t min_train_size,
    const MarkovSwitchingConfig& ms_config,
    bool verbose = true
);


} // namespace statistics
} // namespace trade_ngin

