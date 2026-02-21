#include "trade_ngin/statistics/preprocessing/missing_data_handler.hpp"
#include <cmath>

namespace trade_ngin {
namespace statistics {

MissingDataHandler::MissingDataHandler(MissingDataHandlerConfig config)
    : config_(config) {}

size_t MissingDataHandler::count_missing(const std::vector<double>& data) {
    size_t count = 0;
    for (double v : data) {
        if (std::isnan(v)) ++count;
    }
    return count;
}

size_t MissingDataHandler::count_missing(const Eigen::MatrixXd& data) {
    size_t count = 0;
    for (int i = 0; i < data.rows(); ++i) {
        for (int j = 0; j < data.cols(); ++j) {
            if (std::isnan(data(i, j))) ++count;
        }
    }
    return count;
}

Result<void> MissingDataHandler::validate(const std::vector<double>& data) const {
    if (data.empty()) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
            "Input data is empty", "MissingDataHandler");
    }
    return Result<void>();
}

Result<void> MissingDataHandler::validate(const Eigen::MatrixXd& data) const {
    if (data.rows() == 0 || data.cols() == 0) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
            "Input matrix is empty", "MissingDataHandler");
    }
    return Result<void>();
}

Result<std::vector<double>> MissingDataHandler::handle(const std::vector<double>& data) const {
    {
        auto valid = validate(data);
        if (valid.is_error()) {
            return make_error<std::vector<double>>(valid.error()->code(), valid.error()->what(), "MissingDataHandler");
        }
    }

    size_t n_missing = count_missing(data);

    switch (config_.strategy) {
        case MissingDataHandlerConfig::Strategy::ERROR: {
            if (n_missing > 0) {
                return make_error<std::vector<double>>(ErrorCode::INVALID_DATA,
                    "NaN detected in data (" + std::to_string(n_missing) + " missing values)",
                    "MissingDataHandler");
            }
            return Result<std::vector<double>>(data);
        }

        case MissingDataHandlerConfig::Strategy::DROP: {
            std::vector<double> result;
            result.reserve(data.size() - n_missing);
            for (double v : data) {
                if (!std::isnan(v)) {
                    result.push_back(v);
                }
            }
            if (result.empty()) {
                return make_error<std::vector<double>>(ErrorCode::INVALID_DATA,
                    "All values are NaN", "MissingDataHandler");
            }
            return Result<std::vector<double>>(std::move(result));
        }

        case MissingDataHandlerConfig::Strategy::FORWARD_FILL: {
            if (std::isnan(data[0])) {
                return make_error<std::vector<double>>(ErrorCode::INVALID_DATA,
                    "Cannot forward-fill: first value is NaN", "MissingDataHandler");
            }
            std::vector<double> result = data;
            for (size_t i = 1; i < result.size(); ++i) {
                if (std::isnan(result[i])) {
                    result[i] = result[i - 1];
                }
            }
            return Result<std::vector<double>>(std::move(result));
        }

        case MissingDataHandlerConfig::Strategy::INTERPOLATE: {
            if (n_missing == data.size()) {
                return make_error<std::vector<double>>(ErrorCode::INVALID_DATA,
                    "All values are NaN", "MissingDataHandler");
            }
            std::vector<double> result = data;

            // Find first and last valid indices for edge extrapolation
            size_t first_valid = 0;
            while (first_valid < result.size() && std::isnan(result[first_valid])) ++first_valid;
            size_t last_valid = result.size() - 1;
            while (last_valid > 0 && std::isnan(result[last_valid])) --last_valid;

            // Extrapolate leading NaNs with nearest valid
            for (size_t i = 0; i < first_valid; ++i) {
                result[i] = result[first_valid];
            }

            // Extrapolate trailing NaNs with nearest valid
            for (size_t i = last_valid + 1; i < result.size(); ++i) {
                result[i] = result[last_valid];
            }

            // Linear interpolation for interior NaNs
            size_t i = first_valid + 1;
            while (i <= last_valid) {
                if (std::isnan(result[i])) {
                    // Find next valid value
                    size_t j = i + 1;
                    while (j <= last_valid && std::isnan(result[j])) ++j;

                    // Interpolate between i-1 (valid) and j (valid)
                    double left = result[i - 1];
                    double right = result[j];
                    size_t gap = j - (i - 1);
                    for (size_t k = i; k < j; ++k) {
                        double t = static_cast<double>(k - (i - 1)) / static_cast<double>(gap);
                        result[k] = left + t * (right - left);
                    }
                    i = j + 1;
                } else {
                    ++i;
                }
            }

            return Result<std::vector<double>>(std::move(result));
        }

        case MissingDataHandlerConfig::Strategy::MEAN_FILL: {
            // Compute mean of non-NaN values
            double sum = 0.0;
            size_t valid_count = 0;
            for (double v : data) {
                if (!std::isnan(v)) {
                    sum += v;
                    ++valid_count;
                }
            }
            if (valid_count == 0) {
                return make_error<std::vector<double>>(ErrorCode::INVALID_DATA,
                    "All values are NaN, cannot compute mean", "MissingDataHandler");
            }
            double mean = sum / static_cast<double>(valid_count);

            std::vector<double> result = data;
            for (auto& v : result) {
                if (std::isnan(v)) v = mean;
            }
            return Result<std::vector<double>>(std::move(result));
        }
    }

    return make_error<std::vector<double>>(ErrorCode::INVALID_ARGUMENT,
        "Unknown strategy", "MissingDataHandler");
}

Result<Eigen::MatrixXd> MissingDataHandler::handle(const Eigen::MatrixXd& data) const {
    {
        auto valid = validate(data);
        if (valid.is_error()) {
            return make_error<Eigen::MatrixXd>(valid.error()->code(), valid.error()->what(), "MissingDataHandler");
        }
    }

    size_t n_missing = count_missing(data);

    switch (config_.strategy) {
        case MissingDataHandlerConfig::Strategy::ERROR: {
            if (n_missing > 0) {
                return make_error<Eigen::MatrixXd>(ErrorCode::INVALID_DATA,
                    "NaN detected in matrix data (" + std::to_string(n_missing) + " missing values)",
                    "MissingDataHandler");
            }
            return Result<Eigen::MatrixXd>(data);
        }

        case MissingDataHandlerConfig::Strategy::DROP: {
            // Drop rows with any NaN
            std::vector<int> valid_rows;
            for (int i = 0; i < data.rows(); ++i) {
                bool has_nan = false;
                for (int j = 0; j < data.cols(); ++j) {
                    if (std::isnan(data(i, j))) {
                        has_nan = true;
                        break;
                    }
                }
                if (!has_nan) valid_rows.push_back(i);
            }
            if (valid_rows.empty()) {
                return make_error<Eigen::MatrixXd>(ErrorCode::INVALID_DATA,
                    "All rows contain NaN", "MissingDataHandler");
            }
            Eigen::MatrixXd result(static_cast<int>(valid_rows.size()), data.cols());
            for (int i = 0; i < static_cast<int>(valid_rows.size()); ++i) {
                result.row(i) = data.row(valid_rows[i]);
            }
            return Result<Eigen::MatrixXd>(std::move(result));
        }

        case MissingDataHandlerConfig::Strategy::FORWARD_FILL: {
            // Check first row
            for (int j = 0; j < data.cols(); ++j) {
                if (std::isnan(data(0, j))) {
                    return make_error<Eigen::MatrixXd>(ErrorCode::INVALID_DATA,
                        "Cannot forward-fill: first row contains NaN", "MissingDataHandler");
                }
            }
            Eigen::MatrixXd result = data;
            for (int i = 1; i < result.rows(); ++i) {
                for (int j = 0; j < result.cols(); ++j) {
                    if (std::isnan(result(i, j))) {
                        result(i, j) = result(i - 1, j);
                    }
                }
            }
            return Result<Eigen::MatrixXd>(std::move(result));
        }

        case MissingDataHandlerConfig::Strategy::INTERPOLATE: {
            Eigen::MatrixXd result = data;
            for (int j = 0; j < result.cols(); ++j) {
                // Extract column as vector
                std::vector<double> col(result.rows());
                for (int i = 0; i < result.rows(); ++i) {
                    col[i] = result(i, j);
                }

                // Check not all NaN
                bool all_nan = true;
                for (double v : col) {
                    if (!std::isnan(v)) { all_nan = false; break; }
                }
                if (all_nan) {
                    return make_error<Eigen::MatrixXd>(ErrorCode::INVALID_DATA,
                        "Column " + std::to_string(j) + " is all NaN", "MissingDataHandler");
                }

                // Apply univariate interpolation
                MissingDataHandlerConfig interp_config;
                interp_config.strategy = MissingDataHandlerConfig::Strategy::INTERPOLATE;
                MissingDataHandler interp_handler(interp_config);
                auto col_result = interp_handler.handle(col);
                if (col_result.is_error()) {
                    return make_error<Eigen::MatrixXd>(col_result.error()->code(),
                        col_result.error()->what(), "MissingDataHandler");
                }
                const auto& filled = col_result.value();
                for (int i = 0; i < result.rows(); ++i) {
                    result(i, j) = filled[i];
                }
            }
            return Result<Eigen::MatrixXd>(std::move(result));
        }

        case MissingDataHandlerConfig::Strategy::MEAN_FILL: {
            Eigen::MatrixXd result = data;
            for (int j = 0; j < result.cols(); ++j) {
                double sum = 0.0;
                int valid_count = 0;
                for (int i = 0; i < result.rows(); ++i) {
                    if (!std::isnan(result(i, j))) {
                        sum += result(i, j);
                        ++valid_count;
                    }
                }
                if (valid_count == 0) {
                    return make_error<Eigen::MatrixXd>(ErrorCode::INVALID_DATA,
                        "Column " + std::to_string(j) + " is all NaN", "MissingDataHandler");
                }
                double mean = sum / valid_count;
                for (int i = 0; i < result.rows(); ++i) {
                    if (std::isnan(result(i, j))) {
                        result(i, j) = mean;
                    }
                }
            }
            return Result<Eigen::MatrixXd>(std::move(result));
        }
    }

    return make_error<Eigen::MatrixXd>(ErrorCode::INVALID_ARGUMENT,
        "Unknown strategy", "MissingDataHandler");
}

} // namespace statistics
} // namespace trade_ngin
