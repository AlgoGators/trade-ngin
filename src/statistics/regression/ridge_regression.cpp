#include "trade_ngin/statistics/regression/ridge_regression.hpp"
#include "trade_ngin/statistics/validation.hpp"
#include "trade_ngin/core/logger.hpp"
#include <cmath>
#include <algorithm>
#include <numeric>

namespace trade_ngin {
namespace statistics {

RidgeRegression::RidgeRegression(RidgeRegressionConfig config)
    : config_(config) {}

RidgeResult RidgeRegression::fit_alpha(const Eigen::MatrixXd& Xc, const Eigen::VectorXd& yc, double alpha) const {
    int p = Xc.cols();
    Eigen::MatrixXd XtX = Xc.transpose() * Xc;
    XtX += alpha * Eigen::MatrixXd::Identity(p, p);

    Eigen::VectorXd Xty = Xc.transpose() * yc;
    Eigen::VectorXd beta = XtX.ldlt().solve(Xty);

    // R² on centered data
    Eigen::VectorXd residuals = yc - Xc * beta;
    double ss_res = residuals.squaredNorm();
    double ss_tot = yc.squaredNorm();

    RidgeResult res;
    res.coefficients = beta;
    res.r_squared = (ss_tot > 0) ? 1.0 - ss_res / ss_tot : 1.0;
    res.best_alpha = alpha;
    return res;
}

double RidgeRegression::cross_validate(const Eigen::MatrixXd& Xc, const Eigen::VectorXd& yc,
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

Result<RidgeResult> RidgeRegression::fit(const Eigen::MatrixXd& X, const Eigen::VectorXd& y) {
    std::lock_guard<std::mutex> lock(mutex_);

    {
        auto valid = validation::validate_matrix(X, 2, 1, "RidgeRegression");
        if (valid.is_error()) return make_error<RidgeResult>(valid.error()->code(), valid.error()->what(), "RidgeRegression");
    }

    if (X.rows() != y.size()) {
        return make_error<RidgeResult>(ErrorCode::INVALID_ARGUMENT,
            "X rows must match y size", "RidgeRegression");
    }

    if (!y.allFinite()) {
        return make_error<RidgeResult>(ErrorCode::INVALID_DATA, "NaN/Inf detected in y", "RidgeRegression");
    }

    DEBUG("[RidgeRegression::fit] n=" << X.rows() << " p=" << X.cols() << " alpha=" << config_.alpha);

    // Center X and y (don't penalize intercept)
    x_mean_ = X.colwise().mean();
    y_mean_ = y.mean();
    Eigen::MatrixXd Xc = X.rowwise() - x_mean_.transpose();
    Eigen::VectorXd yc = y.array() - y_mean_;

    double best_alpha = config_.alpha;

    // Cross-validation for alpha selection
    if (config_.cv_folds > 1 && !config_.alpha_candidates.empty()) {
        double best_mse = std::numeric_limits<double>::infinity();
        for (double a : config_.alpha_candidates) {
            double mse = cross_validate(Xc, yc, a, config_.cv_folds);
            if (mse < best_mse) {
                best_mse = mse;
                best_alpha = a;
            }
        }
        DEBUG("[RidgeRegression::fit] CV selected alpha=" << best_alpha);
    }

    result_ = fit_alpha(Xc, yc, best_alpha);

    // Compute intercept
    if (config_.include_intercept) {
        result_.intercept = y_mean_ - x_mean_.dot(result_.coefficients);
    }

    result_.best_alpha = best_alpha;
    fitted_ = true;

    return Result<RidgeResult>(result_);
}

Result<Eigen::VectorXd> RidgeRegression::predict(const Eigen::MatrixXd& X) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!fitted_) {
        return make_error<Eigen::VectorXd>(ErrorCode::NOT_INITIALIZED,
            "Ridge model has not been fitted", "RidgeRegression");
    }

    if (X.cols() != result_.coefficients.size()) {
        return make_error<Eigen::VectorXd>(ErrorCode::INVALID_ARGUMENT,
            "Feature dimension mismatch", "RidgeRegression");
    }

    Eigen::VectorXd predictions = X * result_.coefficients;
    if (config_.include_intercept) {
        predictions.array() += result_.intercept;
    }

    return Result<Eigen::VectorXd>(std::move(predictions));
}

} // namespace statistics
} // namespace trade_ngin
