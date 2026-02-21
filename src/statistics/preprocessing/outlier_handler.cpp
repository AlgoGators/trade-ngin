#include "trade_ngin/statistics/preprocessing/outlier_handler.hpp"
#include <algorithm>
#include <cmath>

namespace trade_ngin {
namespace statistics {

OutlierHandler::OutlierHandler(OutlierHandlerConfig config)
    : config_(config) {}

Result<void> OutlierHandler::validate(const std::vector<double>& data) const {
    if (data.empty()) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
            "Input data is empty", "OutlierHandler");
    }

    if (data.size() < 3) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
            "Insufficient data: need at least 3 observations", "OutlierHandler");
    }

    for (size_t i = 0; i < data.size(); ++i) {
        if (std::isnan(data[i]) || std::isinf(data[i])) {
            return make_error<void>(ErrorCode::INVALID_DATA,
                "NaN/Inf detected at index " + std::to_string(i), "OutlierHandler");
        }
    }

    if (config_.method != OutlierHandlerConfig::Method::MAD_FILTER) {
        if (config_.lower_percentile <= 0.0 || config_.lower_percentile >= 1.0 ||
            config_.upper_percentile <= 0.0 || config_.upper_percentile >= 1.0) {
            return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                "Percentiles must be in (0, 1)", "OutlierHandler");
        }
        if (config_.lower_percentile >= config_.upper_percentile) {
            return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                "lower_percentile must be less than upper_percentile", "OutlierHandler");
        }
    } else {
        if (config_.mad_threshold <= 0.0) {
            return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                "MAD threshold must be positive", "OutlierHandler");
        }
    }

    return Result<void>();
}

Result<std::vector<double>> OutlierHandler::handle(const std::vector<double>& data) const {
    auto valid = validate(data);
    if (valid.is_error()) {
        return make_error<std::vector<double>>(valid.error()->code(), valid.error()->what(), "OutlierHandler");
    }

    switch (config_.method) {
        case OutlierHandlerConfig::Method::WINSORIZE:
            return Result<std::vector<double>>(winsorize(data));
        case OutlierHandlerConfig::Method::TRIM: {
            auto indices = detect_percentile(data);
            return Result<std::vector<double>>(trim(data, indices));
        }
        case OutlierHandlerConfig::Method::MAD_FILTER: {
            auto indices = detect_mad(data);
            return Result<std::vector<double>>(mad_filter(data, indices));
        }
    }
    return make_error<std::vector<double>>(ErrorCode::INVALID_ARGUMENT,
        "Unknown outlier handling method", "OutlierHandler");
}

Result<std::vector<size_t>> OutlierHandler::detect(const std::vector<double>& data) const {
    auto valid = validate(data);
    if (valid.is_error()) {
        return make_error<std::vector<size_t>>(valid.error()->code(), valid.error()->what(), "OutlierHandler");
    }

    if (config_.method == OutlierHandlerConfig::Method::MAD_FILTER) {
        return Result<std::vector<size_t>>(detect_mad(data));
    } else {
        return Result<std::vector<size_t>>(detect_percentile(data));
    }
}

double OutlierHandler::percentile(const std::vector<double>& sorted_data, double p) {
    double idx = p * static_cast<double>(sorted_data.size() - 1);
    size_t lo = static_cast<size_t>(std::floor(idx));
    size_t hi = static_cast<size_t>(std::ceil(idx));
    if (lo == hi) return sorted_data[lo];
    double frac = idx - static_cast<double>(lo);
    return sorted_data[lo] * (1.0 - frac) + sorted_data[hi] * frac;
}

double OutlierHandler::median(const std::vector<double>& data) {
    std::vector<double> sorted = data;
    std::sort(sorted.begin(), sorted.end());
    size_t n = sorted.size();
    if (n % 2 == 0) {
        return (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;
    }
    return sorted[n / 2];
}

std::vector<size_t> OutlierHandler::detect_percentile(const std::vector<double>& data) const {
    std::vector<double> sorted = data;
    std::sort(sorted.begin(), sorted.end());

    double lower = percentile(sorted, config_.lower_percentile);
    double upper = percentile(sorted, config_.upper_percentile);

    std::vector<size_t> indices;
    for (size_t i = 0; i < data.size(); ++i) {
        if (data[i] < lower || data[i] > upper) {
            indices.push_back(i);
        }
    }
    return indices;
}

std::vector<size_t> OutlierHandler::detect_mad(const std::vector<double>& data) const {
    double med = median(data);

    std::vector<double> abs_deviations(data.size());
    for (size_t i = 0; i < data.size(); ++i) {
        abs_deviations[i] = std::abs(data[i] - med);
    }

    double mad = 1.4826 * median(abs_deviations);

    std::vector<size_t> indices;
    if (mad < 1e-15) {
        // All values are (nearly) the same — no outliers
        return indices;
    }

    double lower = med - config_.mad_threshold * mad;
    double upper = med + config_.mad_threshold * mad;

    for (size_t i = 0; i < data.size(); ++i) {
        if (data[i] < lower || data[i] > upper) {
            indices.push_back(i);
        }
    }
    return indices;
}

std::vector<double> OutlierHandler::winsorize(const std::vector<double>& data) const {
    std::vector<double> sorted = data;
    std::sort(sorted.begin(), sorted.end());

    double lower = percentile(sorted, config_.lower_percentile);
    double upper = percentile(sorted, config_.upper_percentile);

    std::vector<double> result = data;
    for (auto& v : result) {
        if (v < lower) v = lower;
        if (v > upper) v = upper;
    }
    return result;
}

std::vector<double> OutlierHandler::trim(const std::vector<double>& data, const std::vector<size_t>& outlier_indices) const {
    std::vector<bool> is_outlier(data.size(), false);
    for (size_t idx : outlier_indices) {
        is_outlier[idx] = true;
    }

    std::vector<double> result;
    result.reserve(data.size() - outlier_indices.size());
    for (size_t i = 0; i < data.size(); ++i) {
        if (!is_outlier[i]) {
            result.push_back(data[i]);
        }
    }
    return result;
}

std::vector<double> OutlierHandler::mad_filter(const std::vector<double>& data, const std::vector<size_t>& outlier_indices) const {
    double med = median(data);

    std::vector<bool> is_outlier(data.size(), false);
    for (size_t idx : outlier_indices) {
        is_outlier[idx] = true;
    }

    std::vector<double> result = data;
    for (size_t i = 0; i < result.size(); ++i) {
        if (is_outlier[i]) {
            result[i] = med;
        }
    }
    return result;
}

} // namespace statistics
} // namespace trade_ngin
