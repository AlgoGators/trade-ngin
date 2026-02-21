#pragma once

#include "trade_ngin/statistics/base/data_transformer.hpp"
#include "trade_ngin/statistics/statistics_common.hpp"
#include <mutex>

namespace trade_ngin {
namespace statistics {

/**
 * @brief Data normalization transformer
 */
class Normalizer : public DataTransformer {
public:
    explicit Normalizer(NormalizationConfig config);

    Result<void> fit(const Eigen::MatrixXd& data) override;
    Result<Eigen::MatrixXd> transform(const Eigen::MatrixXd& data) const override;
    Result<Eigen::MatrixXd> inverse_transform(const Eigen::MatrixXd& data) const override;
    bool is_fitted() const override { return fitted_; }

    const Eigen::VectorXd& get_mean() const { return mean_; }
    const Eigen::VectorXd& get_std() const { return std_; }

private:
    NormalizationConfig config_;
    Eigen::VectorXd mean_;
    Eigen::VectorXd std_;
    Eigen::VectorXd min_;
    Eigen::VectorXd max_;
    Eigen::VectorXd median_;
    Eigen::VectorXd iqr_;
    bool fitted_{false};
    mutable std::mutex mutex_;
};

} // namespace statistics
} // namespace trade_ngin
