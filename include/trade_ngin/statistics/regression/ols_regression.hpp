#pragma once

#include "trade_ngin/statistics/statistics_common.hpp"
#include <Eigen/Dense>
#include <mutex>

namespace trade_ngin {
namespace statistics {

class OLSRegression {
public:
    explicit OLSRegression(OLSRegressionConfig config = {});

    Result<OLSResult> fit(const Eigen::MatrixXd& X, const Eigen::VectorXd& y);
    Result<Eigen::VectorXd> predict(const Eigen::MatrixXd& X) const;
    bool is_fitted() const { return fitted_; }

    const OLSResult& result() const { return result_; }

private:
    OLSRegressionConfig config_;
    OLSResult result_;
    bool fitted_{false};
    mutable std::mutex mutex_;

    Eigen::MatrixXd prepare_X(const Eigen::MatrixXd& X) const;
};

} // namespace statistics
} // namespace trade_ngin
