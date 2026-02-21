#pragma once

#include "trade_ngin/statistics/statistics_common.hpp"
#include <Eigen/Dense>
#include <mutex>

namespace trade_ngin {
namespace statistics {

class LassoRegression {
public:
    explicit LassoRegression(LassoRegressionConfig config = {});

    Result<LassoResult> fit(const Eigen::MatrixXd& X, const Eigen::VectorXd& y);
    Result<Eigen::VectorXd> predict(const Eigen::MatrixXd& X) const;
    bool is_fitted() const { return fitted_; }

    const LassoResult& result() const { return result_; }
    const ConvergenceInfo& get_convergence_info() const { return last_convergence_info_; }

private:
    LassoRegressionConfig config_;
    LassoResult result_;
    Eigen::VectorXd x_mean_;
    double y_mean_{0.0};
    bool fitted_{false};
    mutable ConvergenceInfo last_convergence_info_;
    mutable std::mutex mutex_;

    LassoResult fit_alpha(const Eigen::MatrixXd& Xc, const Eigen::VectorXd& yc, double alpha) const;
    double cross_validate(const Eigen::MatrixXd& Xc, const Eigen::VectorXd& yc, double alpha, int k) const;
    static double soft_threshold(double x, double lambda);
};

} // namespace statistics
} // namespace trade_ngin
