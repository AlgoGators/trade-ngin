#pragma once

#include "trade_ngin/core/error.hpp"
#include <Eigen/Dense>
#include <cmath>
#include <string>
#include <vector>

namespace trade_ngin {
namespace statistics {
namespace validation {

/**
 * @brief Validate a time series vector for common issues
 * @param data Input time series
 * @param min_length Minimum required length
 * @param component Name of the calling component (for error messages)
 * @return Result<void> indicating success or describing the problem
 */
inline Result<void> validate_time_series(const std::vector<double>& data,
                                         size_t min_length,
                                         const std::string& component) {
    if (data.empty()) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Input time series is empty",
            component
        );
    }

    if (data.size() < min_length) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Insufficient data: got " + std::to_string(data.size()) +
            " observations, need at least " + std::to_string(min_length),
            component
        );
    }

    for (size_t i = 0; i < data.size(); ++i) {
        if (std::isnan(data[i]) || std::isinf(data[i])) {
            return make_error<void>(
                ErrorCode::INVALID_DATA,
                "NaN/Inf detected at index " + std::to_string(i),
                component
            );
        }
    }

    return Result<void>();
}

/**
 * @brief Validate an Eigen matrix for common issues
 * @param mat Input matrix
 * @param min_rows Minimum required rows
 * @param min_cols Minimum required columns
 * @param component Name of the calling component (for error messages)
 * @return Result<void> indicating success or describing the problem
 */
inline Result<void> validate_matrix(const Eigen::MatrixXd& mat,
                                    int min_rows,
                                    int min_cols,
                                    const std::string& component) {
    if (mat.rows() == 0 || mat.cols() == 0) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Input matrix is empty",
            component
        );
    }

    if (mat.rows() < min_rows) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Insufficient rows: got " + std::to_string(mat.rows()) +
            ", need at least " + std::to_string(min_rows),
            component
        );
    }

    if (mat.cols() < min_cols) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Insufficient columns: got " + std::to_string(mat.cols()) +
            ", need at least " + std::to_string(min_cols),
            component
        );
    }

    if (!mat.allFinite()) {
        return make_error<void>(
            ErrorCode::INVALID_DATA,
            "NaN/Inf detected in input matrix",
            component
        );
    }

    return Result<void>();
}

} // namespace validation
} // namespace statistics
} // namespace trade_ngin
