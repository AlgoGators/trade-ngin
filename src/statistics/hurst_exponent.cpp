#include "trade_ngin/statistics/hurst_exponent.hpp"
#include "trade_ngin/statistics/validation.hpp"
#include "trade_ngin/core/logger.hpp"
#include <cmath>
#include <algorithm>
#include <numeric>

namespace trade_ngin {
namespace statistics {

HurstExponent::HurstExponent(HurstExponentConfig config)
    : config_(config) {}

std::pair<double, double> HurstExponent::log_log_ols(const std::vector<double>& log_x,
                                                      const std::vector<double>& log_y) {
    int n = static_cast<int>(log_x.size());
    double sum_x = 0, sum_y = 0, sum_xy = 0, sum_xx = 0;
    for (int i = 0; i < n; ++i) {
        sum_x += log_x[i];
        sum_y += log_y[i];
        sum_xy += log_x[i] * log_y[i];
        sum_xx += log_x[i] * log_x[i];
    }
    double mean_x = sum_x / n;
    double mean_y = sum_y / n;
    double slope = (sum_xy - n * mean_x * mean_y) / (sum_xx - n * mean_x * mean_x);

    // R²
    double ss_tot = 0, ss_res = 0;
    for (int i = 0; i < n; ++i) {
        double predicted = mean_y + slope * (log_x[i] - mean_x);
        ss_res += (log_y[i] - predicted) * (log_y[i] - predicted);
        ss_tot += (log_y[i] - mean_y) * (log_y[i] - mean_y);
    }
    double r_sq = (ss_tot > 0) ? 1.0 - ss_res / ss_tot : 1.0;

    return {slope, r_sq};
}

std::string HurstExponent::interpret(double h) {
    if (h < 0.45) return "mean-reverting";
    if (h > 0.55) return "trending";
    return "random walk";
}

HurstResult HurstExponent::rs_analysis(const std::vector<double>& data) const {
    int N = static_cast<int>(data.size());
    int max_win = (config_.max_window > 0) ? config_.max_window : N / 4;
    int min_win = std::max(config_.min_window, 10);

    // Generate log-spaced window sizes
    std::vector<int> window_sizes;
    double log_min = std::log(static_cast<double>(min_win));
    double log_max = std::log(static_cast<double>(max_win));
    for (int i = 0; i < config_.n_windows; ++i) {
        int w = static_cast<int>(std::exp(log_min + (log_max - log_min) * i / (config_.n_windows - 1)));
        if (window_sizes.empty() || w != window_sizes.back()) {
            window_sizes.push_back(w);
        }
    }

    std::vector<double> log_n, log_rs;

    for (int w : window_sizes) {
        if (w > N) continue;
        int n_segments = N / w;
        if (n_segments < 1) continue;

        double sum_rs = 0.0;
        int valid_segments = 0;

        for (int seg = 0; seg < n_segments; ++seg) {
            int start = seg * w;

            // Calculate mean of segment
            double mean = 0.0;
            for (int i = start; i < start + w; ++i) mean += data[i];
            mean /= w;

            // Cumulative deviation from mean
            std::vector<double> cum_dev(w);
            cum_dev[0] = data[start] - mean;
            for (int i = 1; i < w; ++i) {
                cum_dev[i] = cum_dev[i - 1] + (data[start + i] - mean);
            }

            double R = *std::max_element(cum_dev.begin(), cum_dev.end()) -
                       *std::min_element(cum_dev.begin(), cum_dev.end());

            // Standard deviation
            double var = 0.0;
            for (int i = start; i < start + w; ++i) {
                double d = data[i] - mean;
                var += d * d;
            }
            double S = std::sqrt(var / w);

            if (S > 1e-15) {
                sum_rs += R / S;
                valid_segments++;
            }
        }

        if (valid_segments > 0) {
            log_n.push_back(std::log(static_cast<double>(w)));
            log_rs.push_back(std::log(sum_rs / valid_segments));
        }
    }

    if (log_n.size() < 3) {
        return {0.5, 0.0, "random walk"};
    }

    auto [slope, r_sq] = log_log_ols(log_n, log_rs);

    HurstResult result;
    result.hurst_exponent = slope;
    result.r_squared = r_sq;
    result.interpretation = interpret(slope);
    return result;
}

HurstResult HurstExponent::dfa(const std::vector<double>& data) const {
    int N = static_cast<int>(data.size());

    // Cumulative sum of mean-removed series
    double mean = 0.0;
    for (double v : data) mean += v;
    mean /= N;

    std::vector<double> profile(N);
    profile[0] = data[0] - mean;
    for (int i = 1; i < N; ++i) {
        profile[i] = profile[i - 1] + (data[i] - mean);
    }

    int max_win = (config_.max_window > 0) ? config_.max_window : N / 4;
    int min_win = std::max(config_.min_window, 10);

    std::vector<int> window_sizes;
    double log_min = std::log(static_cast<double>(min_win));
    double log_max = std::log(static_cast<double>(max_win));
    for (int i = 0; i < config_.n_windows; ++i) {
        int w = static_cast<int>(std::exp(log_min + (log_max - log_min) * i / (config_.n_windows - 1)));
        if (window_sizes.empty() || w != window_sizes.back()) {
            window_sizes.push_back(w);
        }
    }

    std::vector<double> log_n, log_f;

    for (int w : window_sizes) {
        if (w > N) continue;
        int n_segments = N / w;
        if (n_segments < 1) continue;

        double sum_sq_fluct = 0.0;
        int valid_segments = 0;

        for (int seg = 0; seg < n_segments; ++seg) {
            int start = seg * w;

            // Fit linear trend to segment using least squares
            double sx = 0, sy = 0, sxy = 0, sxx = 0;
            for (int i = 0; i < w; ++i) {
                double x = static_cast<double>(i);
                double y = profile[start + i];
                sx += x; sy += y; sxy += x * y; sxx += x * x;
            }
            double denom = w * sxx - sx * sx;
            if (std::abs(denom) < 1e-15) continue;
            double a = (w * sxy - sx * sy) / denom;
            double b = (sy - a * sx) / w;

            // Detrended fluctuation
            double fluct = 0.0;
            for (int i = 0; i < w; ++i) {
                double trend = a * i + b;
                double diff = profile[start + i] - trend;
                fluct += diff * diff;
            }
            sum_sq_fluct += fluct / w;
            valid_segments++;
        }

        if (valid_segments > 0) {
            double F = std::sqrt(sum_sq_fluct / valid_segments);
            if (F > 1e-15) {
                log_n.push_back(std::log(static_cast<double>(w)));
                log_f.push_back(std::log(F));
            }
        }
    }

    if (log_n.size() < 3) {
        return {0.5, 0.0, "random walk"};
    }

    auto [slope, r_sq] = log_log_ols(log_n, log_f);

    HurstResult result;
    result.hurst_exponent = slope;
    result.r_squared = r_sq;
    result.interpretation = interpret(slope);
    return result;
}

HurstResult HurstExponent::periodogram(const std::vector<double>& data) const {
    int N = static_cast<int>(data.size());

    // Compute periodogram using DFT
    double mean = 0.0;
    for (double v : data) mean += v;
    mean /= N;

    // Compute spectral density at Fourier frequencies
    int n_freq = N / 2;
    std::vector<double> log_freq, log_power;

    for (int k = 1; k <= n_freq; ++k) {
        double freq = 2.0 * M_PI * k / N;
        double cos_sum = 0.0, sin_sum = 0.0;
        for (int t = 0; t < N; ++t) {
            double v = data[t] - mean;
            cos_sum += v * std::cos(freq * t);
            sin_sum += v * std::sin(freq * t);
        }
        double power = (cos_sum * cos_sum + sin_sum * sin_sum) / N;

        if (power > 1e-15) {
            log_freq.push_back(std::log(freq));
            log_power.push_back(std::log(power));
        }
    }

    // Use only low-frequency portion (first sqrt(N) frequencies)
    int n_use = std::min(static_cast<int>(log_freq.size()),
                         std::max(static_cast<int>(std::sqrt(N)), 5));
    log_freq.resize(n_use);
    log_power.resize(n_use);

    if (log_freq.size() < 3) {
        return {0.5, 0.0, "random walk"};
    }

    auto [slope, r_sq] = log_log_ols(log_freq, log_power);

    // H = (1 - slope) / 2 for fractional noise
    double H = (1.0 - slope) / 2.0;
    H = std::max(0.0, std::min(1.0, H));

    HurstResult result;
    result.hurst_exponent = H;
    result.r_squared = r_sq;
    result.interpretation = interpret(H);
    return result;
}

Result<HurstResult> HurstExponent::compute(const std::vector<double>& data) {
    std::lock_guard<std::mutex> lock(mutex_);

    {
        auto valid = validation::validate_time_series(data, 50, "HurstExponent");
        if (valid.is_error()) return make_error<HurstResult>(valid.error()->code(), valid.error()->what(), "HurstExponent");
    }

    DEBUG("[HurstExponent::compute] n=" << data.size() << " method=" << static_cast<int>(config_.method));

    HurstResult result;
    switch (config_.method) {
        case HurstExponentConfig::Method::RS_ANALYSIS:
            result = rs_analysis(data);
            break;
        case HurstExponentConfig::Method::DFA:
            result = dfa(data);
            break;
        case HurstExponentConfig::Method::PERIODOGRAM:
            result = periodogram(data);
            break;
    }

    return Result<HurstResult>(std::move(result));
}

} // namespace statistics
} // namespace trade_ngin
