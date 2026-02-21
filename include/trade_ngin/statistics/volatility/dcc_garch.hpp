#pragma once

#include "trade_ngin/statistics/statistics_common.hpp"
#include "trade_ngin/statistics/volatility/garch.hpp"
#include <Eigen/Dense>
#include <mutex>

namespace trade_ngin {
namespace statistics {

class DCCGARCH {
public:
    explicit DCCGARCH(DCCGARCHConfig config);

    Result<DCCGARCHResult> fit(const Eigen::MatrixXd& returns);
    Result<Eigen::MatrixXd> get_correlation(int t) const;
    Result<Eigen::MatrixXd> forecast_correlation() const;
    bool is_fitted() const { return fitted_; }

private:
    DCCGARCHConfig config_;
    DCCGARCHResult result_;
    double dcc_a_;
    double dcc_b_;
    Eigen::MatrixXd Q_bar_;             // Unconditional correlation of standardized residuals
    Eigen::MatrixXd last_Q_;            // Last Q_t
    Eigen::VectorXd last_std_resid_;    // Last standardized residual vector
    int n_series_{0};
    bool fitted_{false};
    mutable std::mutex mutex_;

    void estimate_dcc_params(const Eigen::MatrixXd& std_residuals);
    double dcc_log_likelihood(const Eigen::MatrixXd& std_residuals,
                               double a, double b) const;
};

} // namespace statistics
} // namespace trade_ngin
