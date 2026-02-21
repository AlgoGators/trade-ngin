#pragma once

#include "trade_ngin/statistics/base/data_transformer.hpp"
#include "trade_ngin/statistics/statistics_common.hpp"
#include <mutex>

namespace trade_ngin {
namespace statistics {

/**
 * @brief Principal Component Analysis transformer
 */
class PCA : public DataTransformer {
public:
    explicit PCA(PCAConfig config);

    Result<void> fit(const Eigen::MatrixXd& data) override;
    Result<Eigen::MatrixXd> transform(const Eigen::MatrixXd& data) const override;
    Result<Eigen::MatrixXd> inverse_transform(const Eigen::MatrixXd& data) const override;
    bool is_fitted() const override { return fitted_; }

    const Eigen::VectorXd& get_explained_variance() const { return explained_variance_; }
    const Eigen::VectorXd& get_explained_variance_ratio() const { return explained_variance_ratio_; }
    const Eigen::MatrixXd& get_components() const { return components_; }
    int get_n_components() const { return n_components_; }

private:
    PCAConfig config_;
    Eigen::MatrixXd components_;        // Principal components (eigenvectors)
    Eigen::VectorXd explained_variance_;
    Eigen::VectorXd explained_variance_ratio_;
    Eigen::VectorXd mean_;
    int n_components_{0};
    bool fitted_{false};
    mutable std::mutex mutex_;
};

} // namespace statistics
} // namespace trade_ngin
