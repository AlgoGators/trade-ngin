#pragma once

#include "trade_ngin/core/error.hpp"
#include <Eigen/Dense>

namespace trade_ngin {
namespace statistics {

/**
 * @brief Base class for all data transformation operations
 */
class DataTransformer {
public:
    virtual ~DataTransformer() = default;

    /**
     * @brief Fit the transformer to data
     * @param data Input data matrix (samples x features)
     * @return Result indicating success or failure
     */
    virtual Result<void> fit(const Eigen::MatrixXd& data) = 0;

    /**
     * @brief Transform data using fitted parameters
     * @param data Input data matrix
     * @return Transformed data matrix
     */
    virtual Result<Eigen::MatrixXd> transform(const Eigen::MatrixXd& data) const = 0;

    /**
     * @brief Fit and transform in one step
     * @param data Input data matrix
     * @return Transformed data matrix
     */
    virtual Result<Eigen::MatrixXd> fit_transform(const Eigen::MatrixXd& data) {
        auto fit_result = fit(data);
        if (fit_result.is_error()) {
            return make_error<Eigen::MatrixXd>(
                fit_result.error()->code(),
                fit_result.error()->what(),
                "DataTransformer"
            );
        }
        return transform(data);
    }

    /**
     * @brief Inverse transform (if applicable)
     * @param data Transformed data matrix
     * @return Original scale data matrix
     */
    virtual Result<Eigen::MatrixXd> inverse_transform(const Eigen::MatrixXd& data) const = 0;

    /**
     * @brief Check if transformer is fitted
     */
    virtual bool is_fitted() const = 0;
};

} // namespace statistics
} // namespace trade_ngin
