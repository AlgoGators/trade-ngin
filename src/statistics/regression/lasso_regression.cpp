#include "trade_ngin/statistics/regression/lasso_regression.hpp"
#include "trade_ngin/statistics/validation.hpp"
#include "trade_ngin/core/logger.hpp"
#include <cmath>
#include <algorithm>

namespace trade_ngin {
namespace statistics {

LassoRegression::LassoRegression(LassoRegressionConfig config)
    : config_(config) {}

double LassoRegression::soft_threshold(double x, double lambda) {
    if (x > lambda) return x - lambda;
    if (x < -lambda) return x + lambda;
    return 0.0;
}

LassoResult LassoRegression::fit_alpha(const Eigen::MatrixXd& Xc, const Eigen::VectorXd& yc, double alpha) const {
    int n = Xc.rows();
    int p = Xc.cols();

    // Precompute column norms
    Eigen::VectorXd col_norms_sq(p);
    for (int j = 0; j < p; ++j) {
        col_norms_sq(j) = Xc.col(j).squaredNorm();
    }

    Eigen::VectorXd beta = Eigen::VectorXd::Zero(p);
    Eigen::VectorXd residuals = yc;

    for (int iter = 0; iter < config_.max_iterations; ++iter) {
        double max_change = 0.0;

        for (int j = 0; j < p; ++j) {
            if (col_norms_sq(j) < 1e-15) continue;

            double old_beta = beta(j);

            // Partial residual (add back j-th contribution)
            double rho = Xc.col(j).dot(residuals) + col_norms_sq(j) * old_beta;

            // Coordinate descent update with soft thresholding
            beta(j) = soft_threshold(rho, alpha * n) / col_norms_sq(j);

            // Update residuals incrementally
            if (beta(j) != old_beta) {
                residuals += (old_beta - beta(j)) * Xc.col(j);
                max_change = std::max(max_change, std::abs(beta(j) - old_beta));
            }
        }

        if (max_change < config_.tolerance) break;
    }

    // Compute R²
    double ss_res = residuals.squaredNorm();
    double ss_tot = yc.squaredNorm();

    LassoResult res;
    res.coefficients = beta;
    res.r_squared = (ss_tot > 0) ? 1.0 - ss_res / ss_tot : 1.0;
    res.best_alpha = alpha;

    // Track selected features
    res.n_nonzero = 0;
    for (int j = 0; j < p; ++j) {
        if (std::abs(beta(j)) > 1e-10) {
            res.selected_features.push_back(j);
            res.n_nonzero++;
        }
    }

    return res;
}

double LassoRegression::cross_validate(const Eigen::MatrixXd& Xc, const Eigen::VectorXd& yc,
                                        double alpha, int k) const {
    int n = Xc.rows();
    int fold_size = n / k;
    double total_mse = 0.0;

    for (int fold = 0; fold < k; ++fold) {
        int start = fold * fold_size;
        int end = (fold == k - 1) ? n : start + fold_size;
        int val_size = end - start;
        int train_size = n - val_size;

        Eigen::MatrixXd X_train(train_size, Xc.cols());
        Eigen::VectorXd y_train(train_size);
        Eigen::MatrixXd X_val(val_size, Xc.cols());
        Eigen::VectorXd y_val(val_size);

        int idx = 0;
        for (int i = 0; i < n; ++i) {
            if (i >= start && i < end) {
                X_val.row(i - start) = Xc.row(i);
                y_val(i - start) = yc(i);
            } else {
                X_train.row(idx) = Xc.row(i);
                y_train(idx) = yc(i);
                idx++;
            }
        }

        auto res = fit_alpha(X_train, y_train, alpha);
        Eigen::VectorXd preds = X_val * res.coefficients;
        double mse = (y_val - preds).squaredNorm() / val_size;
        total_mse += mse;
    }

    return total_mse / k;
}

Result<LassoResult> LassoRegression::fit(const Eigen::MatrixXd& X, const Eigen::VectorXd& y) {
    std::lock_guard<std::mutex> lock(mutex_);

    {
        auto valid = validation::validate_matrix(X, 2, 1, "LassoRegression");
        if (valid.is_error()) return make_error<LassoResult>(valid.error()->code(), valid.error()->what(), "LassoRegression");
    }

    if (X.rows() != y.size()) {
        return make_error<LassoResult>(ErrorCode::INVALID_ARGUMENT,
            "X rows must match y size", "LassoRegression");
    }

    if (!y.allFinite()) {
        return make_error<LassoResult>(ErrorCode::INVALID_DATA, "NaN/Inf detected in y", "LassoRegression");
    }

    DEBUG("[LassoRegression::fit] n=" << X.rows() << " p=" << X.cols() << " alpha=" << config_.alpha);

    // Center X and y
    x_mean_ = X.colwise().mean();
    y_mean_ = y.mean();
    Eigen::MatrixXd Xc = X.rowwise() - x_mean_.transpose();
    Eigen::VectorXd yc = y.array() - y_mean_;

    double best_alpha = config_.alpha;

    // Cross-validation
    if (config_.cv_folds > 1 && !config_.alpha_candidates.empty()) {
        double best_mse = std::numeric_limits<double>::infinity();
        for (double a : config_.alpha_candidates) {
            double mse = cross_validate(Xc, yc, a, config_.cv_folds);
            if (mse < best_mse) {
                best_mse = mse;
                best_alpha = a;
            }
        }
        DEBUG("[LassoRegression::fit] CV selected alpha=" << best_alpha);
    }

    result_ = fit_alpha(Xc, yc, best_alpha);

    // Compute intercept
    if (config_.include_intercept) {
        result_.intercept = y_mean_ - x_mean_.dot(result_.coefficients);
    }

    result_.best_alpha = best_alpha;
    fitted_ = true;

    return Result<LassoResult>(result_);
}

Result<Eigen::VectorXd> LassoRegression::predict(const Eigen::MatrixXd& X) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!fitted_) {
        return make_error<Eigen::VectorXd>(ErrorCode::NOT_INITIALIZED,
            "Lasso model has not been fitted", "LassoRegression");
    }

    if (X.cols() != result_.coefficients.size()) {
        return make_error<Eigen::VectorXd>(ErrorCode::INVALID_ARGUMENT,
            "Feature dimension mismatch", "LassoRegression");
    }

    Eigen::VectorXd predictions = X * result_.coefficients;
    if (config_.include_intercept) {
        predictions.array() += result_.intercept;
    }

    return Result<Eigen::VectorXd>(std::move(predictions));
}

} // namespace statistics
} // namespace trade_ngin
