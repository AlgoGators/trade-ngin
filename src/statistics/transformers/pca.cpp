#include "trade_ngin/statistics/transformers/pca.hpp"
#include "trade_ngin/statistics/validation.hpp"
#include "trade_ngin/core/logger.hpp"
#include <cmath>

namespace trade_ngin {
namespace statistics {

PCA::PCA(PCAConfig config)
    : config_(config) {}

Result<void> PCA::fit(const Eigen::MatrixXd& data) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto valid = validation::validate_matrix(data, 1, 1, "PCA");
    if (valid.is_error()) return valid;

    DEBUG("[PCA::fit] entry: rows=" << data.rows() << " cols=" << data.cols()
          << " n_components=" << config_.n_components);

    // Center the data
    mean_ = data.colwise().mean();
    Eigen::MatrixXd centered(data.rows(), data.cols());
    for (int i = 0; i < data.cols(); ++i) {
        centered.col(i) = data.col(i).array() - mean_(i);
    }

    // Compute covariance matrix
    Eigen::MatrixXd cov = (centered.transpose() * centered) / (data.rows() - 1);

    // Compute eigendecomposition
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> solver(cov);
    if (solver.info() != Eigen::Success) {
        return make_error<void>(
            ErrorCode::INVALID_DATA,
            "Eigenvalue decomposition failed",
            "PCA"
        );
    }

    // Eigenvalues are in ascending order, we want descending
    Eigen::VectorXd eigenvalues = solver.eigenvalues().reverse();
    Eigen::MatrixXd eigenvectors = solver.eigenvectors().colwise().reverse();

    // Determine number of components
    if (config_.n_components > 0) {
        n_components_ = std::min(config_.n_components,
                                static_cast<int>(eigenvalues.size()));
    } else {
        // Use variance threshold
        double total_variance = eigenvalues.sum();
        double cumsum = 0.0;
        n_components_ = 0;
        for (Eigen::Index i = 0; i < eigenvalues.size(); ++i) {
            cumsum += eigenvalues(i);
            n_components_++;
            if (cumsum / total_variance >= config_.variance_threshold) {
                break;
            }
        }
    }

    // Store components and explained variance
    components_ = eigenvectors.leftCols(n_components_);
    explained_variance_ = eigenvalues.head(n_components_);

    double total_var = eigenvalues.sum();
    explained_variance_ratio_ = explained_variance_ / total_var;

    // Apply whitening if requested
    if (config_.whiten) {
        for (int i = 0; i < n_components_; ++i) {
            components_.col(i) /= std::sqrt(explained_variance_(i));
        }
    }

    fitted_ = true;
    DEBUG("[PCA::fit] exit: n_components=" << n_components_);
    return Result<void>();
}

Result<Eigen::MatrixXd> PCA::transform(const Eigen::MatrixXd& data) const {
    std::lock_guard<std::mutex> lock(mutex_);

    {
        auto valid = validation::validate_matrix(data, 1, 1, "PCA");
        if (valid.is_error()) {
            return make_error<Eigen::MatrixXd>(valid.error()->code(), valid.error()->what(), "PCA");
        }
    }

    if (!fitted_) {
        return make_error<Eigen::MatrixXd>(
            ErrorCode::NOT_INITIALIZED,
            "PCA has not been fitted",
            "PCA"
        );
    }

    // Center and project
    Eigen::MatrixXd centered(data.rows(), data.cols());
    for (int i = 0; i < data.cols(); ++i) {
        centered.col(i) = data.col(i).array() - mean_(i);
    }
    Eigen::MatrixXd transformed = centered * components_;

    return Result<Eigen::MatrixXd>(std::move(transformed));
}

Result<Eigen::MatrixXd> PCA::inverse_transform(const Eigen::MatrixXd& data) const {
    std::lock_guard<std::mutex> lock(mutex_);

    {
        auto valid = validation::validate_matrix(data, 1, 1, "PCA");
        if (valid.is_error()) {
            return make_error<Eigen::MatrixXd>(valid.error()->code(), valid.error()->what(), "PCA");
        }
    }

    if (!fitted_) {
        return make_error<Eigen::MatrixXd>(
            ErrorCode::NOT_INITIALIZED,
            "PCA has not been fitted",
            "PCA"
        );
    }

    // Project back and add mean
    Eigen::MatrixXd reconstructed = data * components_.transpose();
    for (int i = 0; i < reconstructed.cols(); ++i) {
        reconstructed.col(i).array() += mean_(i);
    }

    return Result<Eigen::MatrixXd>(std::move(reconstructed));
}

} // namespace statistics
} // namespace trade_ngin
