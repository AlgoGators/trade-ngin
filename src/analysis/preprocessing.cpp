#include "trade_ngin/analysis/preprocessing.hpp"
#include <algorithm>
#include <numeric>
#include <cmath>

namespace trade_ngin {
namespace analysis {

namespace {
    // Helper function to calculate mean
    double calculate_mean(const std::vector<double>& data) {
        return std::accumulate(data.begin(), data.end(), 0.0) / data.size();
    }

    // Helper function to calculate standard deviation
    double calculate_std(const std::vector<double>& data, double mean) {
        double variance = 0.0;
        for (double val : data) {
            variance += (val - mean) * (val - mean);
        }
        return std::sqrt(variance / data.size());
    }

    // Helper function to calculate median
    double calculate_median(std::vector<double> data) {
        std::sort(data.begin(), data.end());
        size_t n = data.size();
        if (n % 2 == 0) {
            return (data[n/2 - 1] + data[n/2]) / 2.0;
        }
        return data[n/2];
    }

    // Helper function to calculate interquartile range
    double calculate_iqr(const std::vector<double>& data) {
        std::vector<double> sorted = data;
        std::sort(sorted.begin(), sorted.end());
        size_t n = sorted.size();

        size_t q1_idx = n / 4;
        size_t q3_idx = 3 * n / 4;

        return sorted[q3_idx] - sorted[q1_idx];
    }
}

// Z-score normalization
Result<std::vector<double>> Normalization::z_score(const std::vector<double>& data) {
    if (data.empty()) {
        return make_error<std::vector<double>>(
            ErrorCode::INVALID_ARGUMENT,
            "Input data cannot be empty",
            "Normalization::z_score"
        );
    }

    double mean = calculate_mean(data);
    double std = calculate_std(data, mean);

    if (std < 1e-10) {
        return make_error<std::vector<double>>(
            ErrorCode::INVALID_ARGUMENT,
            "Standard deviation is too close to zero",
            "Normalization::z_score"
        );
    }

    std::vector<double> result(data.size());
    for (size_t i = 0; i < data.size(); ++i) {
        result[i] = (data[i] - mean) / std;
    }

    return Result<std::vector<double>>(std::move(result));
}

// Min-max normalization
Result<std::vector<double>> Normalization::min_max(const std::vector<double>& data) {
    return min_max_range(data, 0.0, 1.0);
}

// Min-max normalization with custom range
Result<std::vector<double>> Normalization::min_max_range(const std::vector<double>& data,
                                                          double min_val, double max_val) {
    if (data.empty()) {
        return make_error<std::vector<double>>(
            ErrorCode::INVALID_ARGUMENT,
            "Input data cannot be empty",
            "Normalization::min_max_range"
        );
    }

    if (min_val >= max_val) {
        return make_error<std::vector<double>>(
            ErrorCode::INVALID_ARGUMENT,
            "min_val must be less than max_val",
            "Normalization::min_max_range"
        );
    }

    auto [min_it, max_it] = std::minmax_element(data.begin(), data.end());
    double data_min = *min_it;
    double data_max = *max_it;

    if (std::abs(data_max - data_min) < 1e-10) {
        return make_error<std::vector<double>>(
            ErrorCode::INVALID_ARGUMENT,
            "Data range is too small (all values are nearly identical)",
            "Normalization::min_max_range"
        );
    }

    std::vector<double> result(data.size());
    double range = data_max - data_min;
    double target_range = max_val - min_val;

    for (size_t i = 0; i < data.size(); ++i) {
        result[i] = ((data[i] - data_min) / range) * target_range + min_val;
    }

    return Result<std::vector<double>>(std::move(result));
}

// Robust scaling
Result<std::vector<double>> Normalization::robust_scale(const std::vector<double>& data) {
    if (data.empty()) {
        return make_error<std::vector<double>>(
            ErrorCode::INVALID_ARGUMENT,
            "Input data cannot be empty",
            "Normalization::robust_scale"
        );
    }

    double median = calculate_median(data);
    double iqr = calculate_iqr(data);

    if (iqr < 1e-10) {
        return make_error<std::vector<double>>(
            ErrorCode::INVALID_ARGUMENT,
            "IQR is too close to zero",
            "Normalization::robust_scale"
        );
    }

    std::vector<double> result(data.size());
    for (size_t i = 0; i < data.size(); ++i) {
        result[i] = (data[i] - median) / iqr;
    }

    return Result<std::vector<double>>(std::move(result));
}

// Extract closing prices from bars
std::vector<double> Normalization::extract_close_prices(const std::vector<Bar>& bars) {
    std::vector<double> prices;
    prices.reserve(bars.size());
    for (const auto& bar : bars) {
        prices.push_back(bar.close);
    }
    return prices;
}

// Calculate returns
Result<std::vector<double>> Normalization::calculate_returns(const std::vector<double>& prices,
                                                              bool log_returns) {
    if (prices.size() < 2) {
        return make_error<std::vector<double>>(
            ErrorCode::INVALID_ARGUMENT,
            "Need at least 2 prices to calculate returns",
            "Normalization::calculate_returns"
        );
    }

    std::vector<double> returns;
    returns.reserve(prices.size() - 1);

    for (size_t i = 1; i < prices.size(); ++i) {
        if (prices[i-1] <= 0.0) {
            return make_error<std::vector<double>>(
                ErrorCode::INVALID_ARGUMENT,
                "Prices must be positive",
                "Normalization::calculate_returns"
            );
        }

        if (log_returns) {
            returns.push_back(std::log(prices[i] / prices[i-1]));
        } else {
            returns.push_back((prices[i] - prices[i-1]) / prices[i-1]);
        }
    }

    return Result<std::vector<double>>(std::move(returns));
}

// PCA implementation
PCA::PCA(int n_components, bool center, bool scale)
    : n_components_(n_components)
    , center_(center)
    , scale_(scale)
    , fitted_(false) {
}

Result<void> PCA::fit(const Eigen::MatrixXd& data) {
    if (data.rows() < 2) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Need at least 2 samples for PCA",
            "PCA::fit"
        );
    }

    if (data.cols() < 1) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Need at least 1 feature for PCA",
            "PCA::fit"
        );
    }

    int n_samples = data.rows();
    int n_features = data.cols();

    // Determine number of components
    if (n_components_ <= 0) {
        n_components_ = std::min(n_samples, n_features);
    } else {
        n_components_ = std::min(n_components_, std::min(n_samples, n_features));
    }

    // Compute mean
    mean_ = data.colwise().mean();

    // Compute standard deviation if scaling
    if (scale_) {
        std_ = ((data.rowwise() - mean_.transpose()).array().square().colwise().sum() /
                (n_samples - 1)).sqrt();

        // Avoid division by zero
        for (int i = 0; i < std_.size(); ++i) {
            if (std_(i) < 1e-10) {
                std_(i) = 1.0;
            }
        }
    } else {
        std_ = Eigen::VectorXd::Ones(n_features);
    }

    // Preprocess data
    auto preprocessed_result = preprocess(data);
    if (preprocessed_result.is_error()) {
        return make_error<void>(
            preprocessed_result.error()->code(),
            preprocessed_result.error()->what(),
            "PCA::fit"
        );
    }
    Eigen::MatrixXd X = preprocessed_result.value();

    // Perform SVD
    Eigen::JacobiSVD<Eigen::MatrixXd> svd(X, Eigen::ComputeThinU | Eigen::ComputeThinV);

    // Extract components (right singular vectors)
    components_ = svd.matrixV().leftCols(n_components_);

    // Calculate explained variance
    Eigen::VectorXd singular_values = svd.singularValues();
    explained_variance_ = (singular_values.head(n_components_).array().square() /
                          (n_samples - 1)).matrix();

    // Calculate explained variance ratio
    double total_variance = (singular_values.array().square() / (n_samples - 1)).sum();
    explained_variance_ratio_ = explained_variance_ / total_variance;

    fitted_ = true;
    return Result<void>();
}

Result<void> PCA::fit(const std::vector<std::vector<double>>& data) {
    if (data.empty()) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Input data cannot be empty",
            "PCA::fit"
        );
    }

    int n_samples = data.size();
    int n_features = data[0].size();

    // Convert to Eigen matrix
    Eigen::MatrixXd mat(n_samples, n_features);
    for (int i = 0; i < n_samples; ++i) {
        if (static_cast<int>(data[i].size()) != n_features) {
            return make_error<void>(
                ErrorCode::INVALID_ARGUMENT,
                "All samples must have the same number of features",
                "PCA::fit"
            );
        }
        for (int j = 0; j < n_features; ++j) {
            mat(i, j) = data[i][j];
        }
    }

    return fit(mat);
}

Result<Eigen::MatrixXd> PCA::transform(const Eigen::MatrixXd& data) const {
    if (!fitted_) {
        return make_error<Eigen::MatrixXd>(
            ErrorCode::NOT_INITIALIZED,
            "PCA must be fitted before transform",
            "PCA::transform"
        );
    }

    if (data.cols() != mean_.size()) {
        return make_error<Eigen::MatrixXd>(
            ErrorCode::INVALID_ARGUMENT,
            "Number of features does not match fitted model",
            "PCA::transform"
        );
    }

    // Preprocess data
    auto preprocessed_result = preprocess(data);
    if (preprocessed_result.is_error()) {
        return make_error<Eigen::MatrixXd>(
            preprocessed_result.error()->code(),
            preprocessed_result.error()->what(),
            "PCA::transform"
        );
    }
    Eigen::MatrixXd X = preprocessed_result.value();

    // Project onto principal components
    Eigen::MatrixXd transformed = X * components_;

    return Result<Eigen::MatrixXd>(std::move(transformed));
}

Result<PCAResult> PCA::fit_transform(const Eigen::MatrixXd& data) {
    auto fit_result = fit(data);
    if (fit_result.is_error()) {
        return make_error<PCAResult>(
            fit_result.error()->code(),
            fit_result.error()->what(),
            "PCA::fit_transform"
        );
    }

    auto transform_result = transform(data);
    if (transform_result.is_error()) {
        return make_error<PCAResult>(
            transform_result.error()->code(),
            transform_result.error()->what(),
            "PCA::fit_transform"
        );
    }

    PCAResult result;
    result.transformed_data = transform_result.value();
    result.components = components_;
    result.explained_variance = explained_variance_;
    result.explained_variance_ratio = explained_variance_ratio_;
    result.total_variance = explained_variance_.sum();

    return Result<PCAResult>(std::move(result));
}

Result<Eigen::MatrixXd> PCA::inverse_transform(const Eigen::MatrixXd& transformed_data) const {
    if (!fitted_) {
        return make_error<Eigen::MatrixXd>(
            ErrorCode::NOT_INITIALIZED,
            "PCA must be fitted before inverse_transform",
            "PCA::inverse_transform"
        );
    }

    if (transformed_data.cols() != n_components_) {
        return make_error<Eigen::MatrixXd>(
            ErrorCode::INVALID_ARGUMENT,
            "Number of components does not match fitted model",
            "PCA::inverse_transform"
        );
    }

    // Reconstruct in standardized space
    Eigen::MatrixXd reconstructed = transformed_data * components_.transpose();

    // Reverse scaling and centering
    if (scale_) {
        reconstructed = reconstructed.array().rowwise() * std_.transpose().array();
    }
    if (center_) {
        reconstructed = reconstructed.rowwise() + mean_.transpose();
    }

    return Result<Eigen::MatrixXd>(std::move(reconstructed));
}

Result<Eigen::MatrixXd> PCA::preprocess(const Eigen::MatrixXd& data) const {
    Eigen::MatrixXd X = data;

    // Center the data
    if (center_) {
        X = X.rowwise() - mean_.transpose();
    }

    // Scale the data
    if (scale_) {
        X = X.array().rowwise() / std_.transpose().array();
    }

    return Result<Eigen::MatrixXd>(std::move(X));
}

} // namespace analysis
} // namespace trade_ngin
