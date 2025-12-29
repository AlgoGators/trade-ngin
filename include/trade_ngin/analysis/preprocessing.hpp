#pragma once

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include <vector>
#include <Eigen/Dense>

namespace trade_ngin {
namespace analysis {

/**
 * @brief Result structure for PCA transformation
 */
struct PCAResult {
    Eigen::MatrixXd transformed_data;    // Transformed data in principal component space
    Eigen::MatrixXd components;          // Principal components (eigenvectors)
    Eigen::VectorXd explained_variance;  // Variance explained by each component
    Eigen::VectorXd explained_variance_ratio;  // Proportion of variance explained
    Eigen::VectorXd singular_values;     // Singular values from SVD
    double total_variance;               // Total variance in the data
};

/**
 * @brief Data normalization and standardization functions
 *
 * Provides simple functional interfaces for common preprocessing operations
 * on price series and feature vectors.
 */
namespace Normalization {

/**
 * @brief Z-score normalization (standardization)
 * @param data Input vector
 * @return Standardized vector with mean=0, std=1
 */
Result<std::vector<double>> z_score(const std::vector<double>& data);

/**
 * @brief Min-max normalization to [0, 1] range
 * @param data Input vector
 * @return Normalized vector in [0, 1]
 */
Result<std::vector<double>> min_max(const std::vector<double>& data);

/**
 * @brief Min-max normalization to custom range
 * @param data Input vector
 * @param min_val Target minimum value
 * @param max_val Target maximum value
 * @return Normalized vector in [min_val, max_val]
 */
Result<std::vector<double>> min_max_range(const std::vector<double>& data,
                                          double min_val, double max_val);

/**
 * @brief Robust scaling using median and IQR
 * @param data Input vector
 * @return Scaled vector using (x - median) / IQR
 */
Result<std::vector<double>> robust_scale(const std::vector<double>& data);

/**
 * @brief Extract closing prices from Bar vector
 * @param bars Vector of OHLCV bars
 * @return Vector of closing prices
 */
std::vector<double> extract_close_prices(const std::vector<Bar>& bars);

/**
 * @brief Extract returns from price series
 * @param prices Vector of prices
 * @param log_returns If true, compute log returns; otherwise simple returns
 * @return Vector of returns (length = prices.size() - 1)
 */
Result<std::vector<double>> calculate_returns(const std::vector<double>& prices,
                                               bool log_returns = false);

} // namespace Normalization

/**
 * @brief Principal Component Analysis (PCA)
 *
 * Class-based interface for stateful PCA operations including
 * fitting, transforming, and inverse transforming data.
 */
class PCA {
public:
    /**
     * @brief Constructor
     * @param n_components Number of principal components to keep (0 = all)
     * @param center Whether to center the data (subtract mean)
     * @param scale Whether to scale the data to unit variance
     */
    explicit PCA(int n_components = 0, bool center = true, bool scale = false);

    /**
     * @brief Fit PCA model to data
     * @param data Input matrix (rows = samples, columns = features)
     * @return Result indicating success or error
     */
    Result<void> fit(const Eigen::MatrixXd& data);

    /**
     * @brief Fit PCA model to data from vector of vectors
     * @param data Input data (outer vector = samples, inner vector = features)
     * @return Result indicating success or error
     */
    Result<void> fit(const std::vector<std::vector<double>>& data);

    /**
     * @brief Transform data to principal component space
     * @param data Input matrix to transform
     * @return Transformed data in PC space
     */
    Result<Eigen::MatrixXd> transform(const Eigen::MatrixXd& data) const;

    /**
     * @brief Fit and transform data in one step
     * @param data Input matrix
     * @return PCA result with transformed data and statistics
     */
    Result<PCAResult> fit_transform(const Eigen::MatrixXd& data);

    /**
     * @brief Inverse transform from PC space back to original space
     * @param transformed_data Data in PC space
     * @return Reconstructed data in original space
     */
    Result<Eigen::MatrixXd> inverse_transform(const Eigen::MatrixXd& transformed_data) const;

    /**
     * @brief Get the principal components (eigenvectors)
     * @return Matrix where each column is a principal component
     */
    const Eigen::MatrixXd& get_components() const { return components_; }

    /**
     * @brief Get explained variance for each component
     * @return Vector of variances
     */
    const Eigen::VectorXd& get_explained_variance() const { return explained_variance_; }

    /**
     * @brief Get explained variance ratio for each component
     * @return Vector of variance ratios (sums to 1.0)
     */
    const Eigen::VectorXd& get_explained_variance_ratio() const { return explained_variance_ratio_; }

    /**
     * @brief Get mean vector used for centering
     * @return Mean vector
     */
    const Eigen::VectorXd& get_mean() const { return mean_; }

    /**
     * @brief Get standard deviation vector used for scaling
     * @return Standard deviation vector
     */
    const Eigen::VectorXd& get_std() const { return std_; }

    /**
     * @brief Check if PCA has been fitted
     * @return True if fit() has been called
     */
    bool is_fitted() const { return fitted_; }

private:
    int n_components_;
    bool center_;
    bool scale_;
    bool fitted_;

    Eigen::MatrixXd components_;
    Eigen::VectorXd explained_variance_;
    Eigen::VectorXd explained_variance_ratio_;
    Eigen::VectorXd mean_;
    Eigen::VectorXd std_;

    Result<Eigen::MatrixXd> preprocess(const Eigen::MatrixXd& data) const;
};

} // namespace analysis
} // namespace trade_ngin
