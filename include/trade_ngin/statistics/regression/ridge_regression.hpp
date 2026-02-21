#pragma once

#include "trade_ngin/statistics/statistics_common.hpp"
#include <Eigen/Dense>
#include <mutex>

namespace trade_ngin {
namespace statistics {

class RidgeRegression {
public:
    explicit RidgeRegression(RidgeRegressionConfig config = {});

    Result<RidgeResult> fit(const Eigen::MatrixXd& X, const Eigen::VectorXd& y);
    Result<Eigen::VectorXd> predict(const Eigen::MatrixXd& X) const;
    bool is_fitted() const { return fitted_; }

    const RidgeResult& result() const { return result_; }

private:
    RidgeRegressionConfig config_;
    RidgeResult result_;
    Eigen::VectorXd x_mean_;
    double y_mean_{0.0};
    bool fitted_{false};
    mutable std::mutex mutex_;

    RidgeResult fit_alpha(const Eigen::MatrixXd& Xc, const Eigen::VectorXd& yc, double alpha) const;
    double cross_validate(const Eigen::MatrixXd& Xc, const Eigen::VectorXd& yc, double alpha, int k) const;
};

} // namespace statistics
} // namespace trade_ngin
