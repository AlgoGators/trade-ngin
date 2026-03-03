#include "trade_ngin/statistics/transformers/normalizer.hpp"
#include "trade_ngin/statistics/statistics_utils.hpp"
#include "trade_ngin/statistics/validation.hpp"
#include "trade_ngin/core/logger.hpp"

namespace trade_ngin {
namespace statistics {

Normalizer::Normalizer(NormalizationConfig config)
    : config_(config) {}

Result<void> Normalizer::fit(const Eigen::MatrixXd& data) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto valid = validation::validate_matrix(data, 1, 1, "Normalizer");
    if (valid.is_error()) return valid;

    DEBUG("[Normalizer::fit] entry: rows=" << data.rows() << " cols=" << data.cols()
          << " method=" << static_cast<int>(config_.method));

    switch (config_.method) {
        case NormalizationConfig::Method::Z_SCORE: {
            mean_ = data.colwise().mean();
            // Compute std column by column
            std_ = Eigen::VectorXd(data.cols());
            for (int i = 0; i < data.cols(); ++i) {
                Eigen::VectorXd centered_col = data.col(i).array() - mean_(i);
                std_(i) = std::sqrt(centered_col.squaredNorm() / (data.rows() - 1));
                // Prevent division by zero
                if (std_(i) < 1e-10) std_(i) = 1.0;
            }
            break;
        }

        case NormalizationConfig::Method::MIN_MAX:
            min_ = data.colwise().minCoeff();
            max_ = data.colwise().maxCoeff();
            // Prevent division by zero
            for (Eigen::Index i = 0; i < min_.size(); ++i) {
                if (std::abs(max_(i) - min_(i)) < 1e-10) {
                    max_(i) = min_(i) + 1.0;
                }
            }
            break;

        case NormalizationConfig::Method::ROBUST: {
            int n_features = data.cols();
            median_.resize(n_features);
            iqr_.resize(n_features);

            for (int i = 0; i < n_features; ++i) {
                std::vector<double> col_data(data.rows());
                for (int j = 0; j < data.rows(); ++j) {
                    col_data[j] = data(j, i);
                }
                median_(i) = utils::calculate_median(col_data);
                iqr_(i) = utils::calculate_iqr(col_data);
                if (iqr_(i) < 1e-10) iqr_(i) = 1.0;
            }
            break;
        }
    }

    fitted_ = true;
    DEBUG("[Normalizer::fit] exit: fitted=true");
    return Result<void>();
}

Result<Eigen::MatrixXd> Normalizer::transform(const Eigen::MatrixXd& data) const {
    std::lock_guard<std::mutex> lock(mutex_);

    {
        auto valid = validation::validate_matrix(data, 1, 1, "Normalizer");
        if (valid.is_error()) {
            return make_error<Eigen::MatrixXd>(valid.error()->code(), valid.error()->what(), "Normalizer");
        }
    }

    if (!fitted_) {
        return make_error<Eigen::MatrixXd>(
            ErrorCode::NOT_INITIALIZED,
            "Normalizer has not been fitted",
            "Normalizer"
        );
    }

    if (data.cols() != mean_.size() && config_.method == NormalizationConfig::Method::Z_SCORE) {
        return make_error<Eigen::MatrixXd>(
            ErrorCode::INVALID_ARGUMENT,
            "Data dimensionality does not match fitted model",
            "Normalizer"
        );
    }

    Eigen::MatrixXd result(data.rows(), data.cols());

    switch (config_.method) {
        case NormalizationConfig::Method::Z_SCORE:
            for (int i = 0; i < data.cols(); ++i) {
                result.col(i) = (data.col(i).array() - mean_(i)) / std_(i);
            }
            break;

        case NormalizationConfig::Method::MIN_MAX:
            for (int i = 0; i < data.cols(); ++i) {
                result.col(i) = (data.col(i).array() - min_(i)) / (max_(i) - min_(i));
            }
            break;

        case NormalizationConfig::Method::ROBUST:
            for (int i = 0; i < data.cols(); ++i) {
                result.col(i) = (data.col(i).array() - median_(i)) / iqr_(i);
            }
            break;
    }

    return Result<Eigen::MatrixXd>(std::move(result));
}

Result<Eigen::MatrixXd> Normalizer::inverse_transform(const Eigen::MatrixXd& data) const {
    std::lock_guard<std::mutex> lock(mutex_);

    {
        auto valid = validation::validate_matrix(data, 1, 1, "Normalizer");
        if (valid.is_error()) {
            return make_error<Eigen::MatrixXd>(valid.error()->code(), valid.error()->what(), "Normalizer");
        }
    }

    if (!fitted_) {
        return make_error<Eigen::MatrixXd>(
            ErrorCode::NOT_INITIALIZED,
            "Normalizer has not been fitted",
            "Normalizer"
        );
    }

    Eigen::MatrixXd result(data.rows(), data.cols());

    switch (config_.method) {
        case NormalizationConfig::Method::Z_SCORE:
            for (int i = 0; i < data.cols(); ++i) {
                result.col(i) = data.col(i).array() * std_(i) + mean_(i);
            }
            break;

        case NormalizationConfig::Method::MIN_MAX:
            for (int i = 0; i < data.cols(); ++i) {
                result.col(i) = data.col(i).array() * (max_(i) - min_(i)) + min_(i);
            }
            break;

        case NormalizationConfig::Method::ROBUST:
            for (int i = 0; i < data.cols(); ++i) {
                result.col(i) = data.col(i).array() * iqr_(i) + median_(i);
            }
            break;
    }

    return Result<Eigen::MatrixXd>(std::move(result));
}

} // namespace statistics
} // namespace trade_ngin
