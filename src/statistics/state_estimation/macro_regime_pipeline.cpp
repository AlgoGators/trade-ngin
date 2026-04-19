#include "trade_ngin/statistics/state_estimation/macro_regime_pipeline.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <numeric>
#include <set>

namespace trade_ngin {
namespace statistics {

static constexpr double kPi  = 3.14159265358979323846;
static constexpr double kEps = 1e-12;

// ============================================================================
// Regime name lookup
// ============================================================================

static const char* kRegimeNames[kNumMacroRegimes] = {
    "EXPANSION_DISINFLATION",
    "EXPANSION_INFLATIONARY",
    "SLOWDOWN_DISINFLATION",
    "SLOWDOWN_INFLATIONARY",
    "RECESSION_DEFLATIONARY",
    "RECESSION_INFLATIONARY"
};

const char* macro_regime_name(MacroRegimeL1 r) {
    int idx = static_cast<int>(r);
    if (idx >= 0 && idx < kNumMacroRegimes) return kRegimeNames[idx];
    return "UNKNOWN";
}

// ============================================================================
// Config serialisation
// ============================================================================

nlohmann::json MacroRegimePipelineConfig::to_json() const {
    return {
        {"w_dfm",                    w_dfm},
        {"w_msdfm",                  w_msdfm},
        {"w_quadrant",               w_quadrant},
        {"w_bsts",                   w_bsts},
        {"dfm_max_em_iterations",    dfm_max_em_iterations},
        {"growth_factor_idx",        growth_factor_idx},
        {"inflation_factor_idx",     inflation_factor_idx},
        {"growth_factor_sign",       growth_factor_sign},
        {"inflation_factor_sign",    inflation_factor_sign},
        {"bsts_always_contribute",   bsts_always_contribute},
        {"growth_upper_pctile",      growth_upper_pctile},
        {"growth_lower_pctile",      growth_lower_pctile},
        {"inflation_pctile",         inflation_pctile},
        {"fingerprint_tau",          fingerprint_tau},
        {"quadrant_blend_band",      quadrant_blend_band},
        {"base_lambda",              base_lambda},
        {"calm_lambda_scale",        calm_lambda_scale},
        {"shock_lambda_scale",       shock_lambda_scale},
        {"shock_threshold",          shock_threshold},
        {"enter_defensive_thresh",   enter_defensive_thresh},
        {"exit_defensive_thresh",    exit_defensive_thresh},
        {"enter_riskoff_thresh",     enter_riskoff_thresh},
        {"exit_riskoff_thresh",      exit_riskoff_thresh},
        {"enter_aggressive_thresh",  enter_aggressive_thresh},
        {"exit_aggressive_thresh",   exit_aggressive_thresh},
        {"min_dwell_bars",           min_dwell_bars},
        {"dwell_penalty",            dwell_penalty},
        {"growth_columns",           growth_columns},
        {"inflation_columns",        inflation_columns},
        {"credit_columns",           credit_columns},
        {"yield_curve_columns",      yield_curve_columns},
        {"policy_columns",           policy_columns}
    };
}

void MacroRegimePipelineConfig::from_json(const nlohmann::json& j) {
    auto get = [&](const char* k, auto& v) {
        if (j.contains(k)) v = j[k].get<std::decay_t<decltype(v)>>();
    };
    get("w_dfm",                    w_dfm);
    get("w_msdfm",                  w_msdfm);
    get("w_quadrant",               w_quadrant);
    get("w_bsts",                   w_bsts);
    get("dfm_max_em_iterations",    dfm_max_em_iterations);
    get("growth_factor_idx",        growth_factor_idx);
    get("inflation_factor_idx",     inflation_factor_idx);
    get("growth_factor_sign",       growth_factor_sign);
    get("inflation_factor_sign",    inflation_factor_sign);
    get("bsts_always_contribute",   bsts_always_contribute);
    get("growth_upper_pctile",      growth_upper_pctile);
    get("growth_lower_pctile",      growth_lower_pctile);
    get("inflation_pctile",         inflation_pctile);
    get("fingerprint_tau",          fingerprint_tau);
    get("quadrant_blend_band",      quadrant_blend_band);
    get("base_lambda",              base_lambda);
    get("calm_lambda_scale",        calm_lambda_scale);
    get("shock_lambda_scale",       shock_lambda_scale);
    get("shock_threshold",          shock_threshold);
    get("enter_defensive_thresh",   enter_defensive_thresh);
    get("exit_defensive_thresh",    exit_defensive_thresh);
    get("enter_riskoff_thresh",     enter_riskoff_thresh);
    get("exit_riskoff_thresh",      exit_riskoff_thresh);
    get("enter_aggressive_thresh",  enter_aggressive_thresh);
    get("exit_aggressive_thresh",   exit_aggressive_thresh);
    get("min_dwell_bars",           min_dwell_bars);
    get("dwell_penalty",            dwell_penalty);
    get("growth_columns",           growth_columns);
    get("inflation_columns",        inflation_columns);
    get("credit_columns",           credit_columns);
    get("yield_curve_columns",      yield_curve_columns);
    get("policy_columns",           policy_columns);
}

// ============================================================================
// Constructor
// ============================================================================

MacroRegimePipeline::MacroRegimePipeline(MacroRegimePipelineConfig config)
    : config_(std::move(config))
{
    prev_smoothed_.setConstant(1.0 / kNumMacroRegimes);

    // B3: hardcoded quadrant-to-regime mapping table (spec Section B3)
    //
    //  Quadrant 0 = GOLDILOCKS  (high growth, low inflation)
    //  Quadrant 1 = REFLATION   (high growth, high inflation)
    //  Quadrant 2 = DEFLATION   (low growth, low inflation)
    //  Quadrant 3 = STAGFLATION (low growth, high inflation)
    //
    //  Columns: EXP_DIS  EXP_INF  SLO_DIS  SLO_INF  REC_DEF  REC_INF

    quadrant_table_ <<
        0.80, 0.15, 0.05, 0.00, 0.00, 0.00,   // GOLDILOCKS
        0.05, 0.85, 0.00, 0.10, 0.00, 0.00,   // REFLATION
        0.15, 0.00, 0.50, 0.00, 0.35, 0.00,   // DEFLATION
        0.00, 0.15, 0.00, 0.55, 0.00, 0.30;   // STAGFLATION
}

// ============================================================================
// Percentile helper
// ============================================================================

double MacroRegimePipeline::percentile(const std::vector<double>& data, double p) {
    std::vector<double> sorted = data;
    std::sort(sorted.begin(), sorted.end());
    double idx = p * (sorted.size() - 1);
    int lo = static_cast<int>(std::floor(idx));
    int hi = static_cast<int>(std::ceil(idx));
    if (lo == hi) return sorted[lo];
    double frac = idx - lo;
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

// ============================================================================
// Train
// ============================================================================

Result<void> MacroRegimePipeline::train(
    const DFMOutput& dfm_output,
    const MSDFMOutput& msdfm_output,
    const MacroPanel& panel,
    const BSTSOutput& bsts_output)
{
    return train(dfm_output, msdfm_output, panel,
                 bsts_output.regime_posteriors,
                 bsts_output.growth_score,
                 bsts_output.inflation_score);
}

Result<void> MacroRegimePipeline::train(
    const DFMOutput& dfm_output,
    const MSDFMOutput& msdfm_output,
    const MacroPanel& panel,
    const Eigen::MatrixXd& bsts_probs,
    const Eigen::VectorXd& growth_scores,
    const Eigen::VectorXd& inflation_scores)
{
    const int T = dfm_output.T;

    if (T < 20) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
            "Need at least 20 timesteps for training", "MacroRegimePipeline");
    }
    if (msdfm_output.T < 10) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
            "MS-DFM output too short", "MacroRegimePipeline");
    }

    // Compute growth/inflation medians + std for quadrant blending
    {
        std::vector<double> gv(growth_scores.data(),
                               growth_scores.data() + growth_scores.size());
        std::vector<double> iv(inflation_scores.data(),
                               inflation_scores.data() + inflation_scores.size());
        growth_median_ = percentile(gv, 0.50);
        inflation_median_ = percentile(iv, 0.50);

        double g_var = 0, i_var = 0;
        for (auto x : gv) g_var += (x - growth_median_) * (x - growth_median_);
        for (auto x : iv) i_var += (x - inflation_median_) * (x - inflation_median_);
        growth_std_ = std::sqrt(g_var / gv.size() + kEps);
        inflation_std_ = std::sqrt(i_var / iv.size() + kEps);
    }

    // B1: DFM Gaussians
    train_dfm_gaussians(dfm_output);

    // B2: MS-DFM fingerprints
    resolve_column_indices(panel);
    prepare_fingerprint_data(panel);
    train_msdfm_fingerprints(msdfm_output, panel);

    // B4: BSTS fingerprints (for break case)
    train_bsts_fingerprints(bsts_probs, panel);

    // Reset runtime state
    prev_smoothed_.setConstant(1.0 / kNumMacroRegimes);
    current_regime_ = MacroRegimeL1::EXPANSION_DISINFLATION;
    dwell_counter_ = 0;
    update_count_  = 0;
    trained_ = true;

    std::cerr << "[MacroRegimePipeline] Training complete. T=" << T << "\n";
    return Result<void>();
}

// ============================================================================
// B1: DFM Gaussian Training
// ============================================================================

void MacroRegimePipeline::train_dfm_gaussians(const DFMOutput& dfm_output) {
    const int T = dfm_output.T;
    const int K = dfm_output.K;

    // Build factor matrix
    Eigen::MatrixXd F(T, K);
    for (int t = 0; t < T; ++t)
        for (int k = 0; k < K; ++k)
            F(t, k) = dfm_output.factors[t][k];

    // Compute percentile thresholds using configurable factor-to-axis mapping.
    // growth_factor_idx and inflation_factor_idx control which DFM factors
    // are treated as growth and inflation for regime labeling.
    const int gi = config_.growth_factor_idx;
    const int ii = config_.inflation_factor_idx;

    const double g_sign = config_.growth_factor_sign;
    const double i_sign = config_.inflation_factor_sign;

    std::vector<double> growth_vals(T), inflation_vals(T);
    for (int t = 0; t < T; ++t) {
        growth_vals[t]    = g_sign * F(t, gi);
        inflation_vals[t] = i_sign * F(t, ii);
    }

    double growth_hi  = percentile(growth_vals, config_.growth_upper_pctile);
    double growth_lo  = percentile(growth_vals, config_.growth_lower_pctile);
    double infl_split = percentile(inflation_vals, config_.inflation_pctile);

    std::cerr << "[B1] using factor " << gi << " as growth, factor " << ii << " as inflation\n";
    std::cerr << "[B1] growth thresholds: lo=" << growth_lo
              << " hi=" << growth_hi << "\n";
    std::cerr << "[B1] inflation split: " << infl_split << "\n";

    // Label each timestep
    std::vector<int> labels(T);
    for (int t = 0; t < T; ++t) {
        int growth_bucket;  // 0=expansion, 1=slowdown, 2=recession
        double g_val = g_sign * F(t, gi);
        double i_val = i_sign * F(t, ii);
        if (g_val >= growth_hi)       growth_bucket = 0;
        else if (g_val >= growth_lo)  growth_bucket = 1;
        else                           growth_bucket = 2;

        int infl_bucket = (i_val >= infl_split) ? 1 : 0;
        labels[t] = growth_bucket * 2 + infl_bucket;
    }

    // Fit Gaussian per regime
    for (int r = 0; r < kNumMacroRegimes; ++r) {
        std::vector<int> indices;
        for (int t = 0; t < T; ++t)
            if (labels[t] == r) indices.push_back(t);

        dfm_gaussians_[r].n_samples = static_cast<int>(indices.size());

        if (static_cast<int>(indices.size()) < 5) {
            std::cerr << "[B1] Warning: regime " << kRegimeNames[r]
                      << " has " << indices.size() << " samples, using fallback\n";
            dfm_gaussians_[r].mean = Eigen::Vector3d::Zero();
            dfm_gaussians_[r].cov = Eigen::Matrix3d::Identity();
            dfm_gaussians_[r].cov_inv = Eigen::Matrix3d::Identity();
            dfm_gaussians_[r].log_det = 0.0;
            continue;
        }

        // Mean
        Eigen::Vector3d mu = Eigen::Vector3d::Zero();
        for (int idx : indices)
            mu += F.row(idx).head<3>().transpose();
        mu /= static_cast<double>(indices.size());

        // Covariance
        Eigen::Matrix3d cov = Eigen::Matrix3d::Zero();
        for (int idx : indices) {
            Eigen::Vector3d diff = F.row(idx).head<3>().transpose() - mu;
            cov += diff * diff.transpose();
        }
        cov /= static_cast<double>(indices.size() - 1);

        // CALIBRATION NOTE: The spec uses a minimal ridge (1e-6) for numerical
        // stability. In practice this produces Gaussians so tight that the soft
        // assignment degenerates to a near-point-mass (0.96 on one regime).
        // A floor of 0.5*I ensures genuine overlap between adjacent regimes
        // so the output is a meaningful probability distribution, not a hard
        // classifier. This is the variance of the Gaussian in each dimension —
        // 0.5 corresponds to ~0.7 std dev of spread around each regime centroid.
        cov += Eigen::Matrix3d::Identity() * 0.5;

        dfm_gaussians_[r].mean    = mu;
        dfm_gaussians_[r].cov     = cov;
        dfm_gaussians_[r].cov_inv = cov.inverse();
        dfm_gaussians_[r].log_det = std::log(cov.determinant());

        std::cerr << "[B1] " << kRegimeNames[r]
                  << " n=" << indices.size()
                  << " mean=[" << mu.transpose() << "]\n";
    }
}

// ============================================================================
// B1: DFM Mapping (runtime)
// ============================================================================

Eigen::Matrix<double, kNumMacroRegimes, 1>
MacroRegimePipeline::map_dfm(const Eigen::Vector3d& f_t) const {
    Eigen::Matrix<double, kNumMacroRegimes, 1> log_probs;

    for (int r = 0; r < kNumMacroRegimes; ++r) {
        const auto& g = dfm_gaussians_[r];
        Eigen::Vector3d diff = f_t - g.mean;
        double mahal = diff.transpose() * g.cov_inv * diff;
        log_probs(r) = -0.5 * (3.0 * std::log(2.0 * kPi) + g.log_det + mahal);
    }

    // Log-sum-exp normalisation
    double max_log = log_probs.maxCoeff();
    Eigen::Matrix<double, kNumMacroRegimes, 1> probs =
        (log_probs.array() - max_log).exp();
    probs /= probs.sum();

    return probs;
}

// ============================================================================
// B2: MS-DFM Fingerprint Training
// ============================================================================

void MacroRegimePipeline::resolve_column_indices(const MacroPanel& panel) {
    auto resolve = [&](const std::vector<std::string>& names,
                       std::vector<int>& indices) {
        indices.clear();
        for (const auto& name : names) {
            for (int c = 0; c < panel.N; ++c) {
                if (panel.column_names[c] == name) {
                    indices.push_back(c);
                    break;
                }
            }
        }
    };

    resolve(config_.growth_columns, growth_col_idx_);
    resolve(config_.inflation_columns, inflation_col_idx_);
    resolve(config_.credit_columns, credit_col_idx_);
    resolve(config_.yield_curve_columns, yield_col_idx_);
    resolve(config_.policy_columns, policy_col_idx_);

    std::cerr << "[fingerprint] resolved columns: growth="
              << growth_col_idx_.size()
              << " inflation=" << inflation_col_idx_.size()
              << " credit=" << credit_col_idx_.size()
              << " yield=" << yield_col_idx_.size()
              << " policy=" << policy_col_idx_.size() << "\n";
}

// Build the standardized fingerprint panel.
//
// Per the .md spec ("Mean growth momentum, Mean inflation level, ..."):
//   - Growth columns: YoY absolute difference, then z-scored
//   - All other columns: levels, z-scored
//
// Without this, compute_fingerprint averaged raw column values — payrolls
// (~150k) and retail sales (~600k) dominated the growth dimension while
// trending upward over time, which made MS-DFM's mapping conflate "later
// dates" with "higher growth" and lock onto SLOWDOWN_DISINFLATION.
void MacroRegimePipeline::prepare_fingerprint_data(const MacroPanel& panel) {
    const int T = panel.T;
    const int N = panel.N;

    fp_panel_ = Eigen::MatrixXd::Constant(
        T, N, std::numeric_limits<double>::quiet_NaN());

    std::set<int> growth_set(growth_col_idx_.begin(), growth_col_idx_.end());

    int growth_yoy_count = 0;
    int level_count = 0;

    for (int c = 0; c < N; ++c) {
        const bool is_growth = growth_set.count(c) > 0;

        // Step 1: build the transformed series for this column.
        std::vector<double> series(T, std::numeric_limits<double>::quiet_NaN());
        if (is_growth) {
            // YoY absolute difference (momentum)
            for (int t = kYoYLag; t < T; ++t) {
                double v_now = panel.data(t, c);
                double v_lag = panel.data(t - kYoYLag, c);
                if (std::isfinite(v_now) && std::isfinite(v_lag))
                    series[t] = v_now - v_lag;
            }
            ++growth_yoy_count;
        } else {
            // Level
            for (int t = 0; t < T; ++t) {
                double v = panel.data(t, c);
                if (std::isfinite(v)) series[t] = v;
            }
            ++level_count;
        }

        // Step 2: per-column mean and std over the transformed series.
        double sum = 0.0, sum_sq = 0.0;
        int count = 0;
        for (int t = 0; t < T; ++t) {
            double v = series[t];
            if (std::isfinite(v)) {
                sum += v;
                sum_sq += v * v;
                ++count;
            }
        }
        if (count < 2) continue;  // not enough data — leave column as NaN

        double mean = sum / count;
        double var = (sum_sq - count * mean * mean) / (count - 1);
        double stdv = std::sqrt(std::max(var, kEps));

        // Step 3: z-score and store
        for (int t = 0; t < T; ++t) {
            if (std::isfinite(series[t]))
                fp_panel_(t, c) = (series[t] - mean) / stdv;
        }
    }

    std::cerr << "[fingerprint] prepared standardized panel: T=" << T
              << " N=" << N
              << " (growth_yoy=" << growth_yoy_count
              << " level=" << level_count
              << ", lookback=" << kYoYLag << " bars)\n";
}

Eigen::VectorXd MacroRegimePipeline::compute_fingerprint(
    const MacroPanel& /*panel*/,
    const std::vector<int>& time_indices) const
{
    // 5D fingerprint: [growth_momentum, inflation_level, credit_spread,
    //                  yield_curve, policy_stance]
    //
    // Reads from fp_panel_ (built by prepare_fingerprint_data), where every
    // column is z-scored and growth columns are YoY-differenced first.
    Eigen::VectorXd fp = Eigen::VectorXd::Zero(kFingerprintDim);
    if (time_indices.empty() || fp_panel_.size() == 0) return fp;

    auto col_mean = [&](const std::vector<int>& col_idx,
                        const std::vector<int>& t_idx) -> double {
        double sum = 0;
        int count = 0;
        for (int t : t_idx) {
            if (t < 0 || t >= fp_panel_.rows()) continue;
            for (int c : col_idx) {
                if (c < 0 || c >= fp_panel_.cols()) continue;
                double v = fp_panel_(t, c);
                if (std::isfinite(v)) {
                    sum += v;
                    ++count;
                }
            }
        }
        return count > 0 ? sum / count : 0.0;
    };

    fp(0) = col_mean(growth_col_idx_, time_indices);
    fp(1) = col_mean(inflation_col_idx_, time_indices);
    fp(2) = col_mean(credit_col_idx_, time_indices);
    fp(3) = col_mean(yield_col_idx_, time_indices);
    fp(4) = col_mean(policy_col_idx_, time_indices);

    return fp;
}

void MacroRegimePipeline::train_msdfm_fingerprints(
    const MSDFMOutput& msdfm_output,
    const MacroPanel& panel)
{
    const int J = msdfm_output.J;
    const int T_ms = msdfm_output.T;
    const int T_panel = panel.T;
    const int T = std::min(T_ms, T_panel);

    // Compute fingerprints for each native state
    msdfm_mapping_.native_fingerprints.resize(J);
    for (int j = 0; j < J; ++j) {
        std::vector<int> indices;
        for (int t = 0; t < T; ++t) {
            if (msdfm_output.decoded_regimes[t] == j)
                indices.push_back(t);
        }
        msdfm_mapping_.native_fingerprints[j] = compute_fingerprint(panel, indices);
        std::cerr << "[B2] native state " << j << " n=" << indices.size()
                  << " fp=[" << msdfm_mapping_.native_fingerprints[j].transpose()
                  << "]\n";
    }

    // Stage 2: cross-state standardisation.
    // After Stage 1 (per-column z-scoring inside compute_fingerprint), each
    // native fingerprint is already in z-score space, but its magnitude is
    // typically ~±0.2 because we average ~thousands of z-scored values per
    // column and ~5 columns per dimension. The target fingerprints are
    // designed at ±1.5 magnitude, so without re-scaling all natives cluster
    // near origin and map to whichever target is closest to origin (SLO_DIS).
    //
    // We re-standardise each dimension across the J native fingerprints
    // so the cross-state spread becomes comparable to the target spread.
    // This is the same J=3 standardization the original code did, but now
    // applied AFTER Stage 1 fixes the unit-mixing/time-trend artifacts.
    {
        Eigen::VectorXd ns_mean = Eigen::VectorXd::Zero(kFingerprintDim);
        for (int j = 0; j < J; ++j) ns_mean += msdfm_mapping_.native_fingerprints[j];
        ns_mean /= J;

        Eigen::VectorXd ns_std = Eigen::VectorXd::Ones(kFingerprintDim);
        if (J >= 2) {
            for (int d = 0; d < kFingerprintDim; ++d) {
                double sum_sq = 0;
                for (int j = 0; j < J; ++j) {
                    double diff = msdfm_mapping_.native_fingerprints[j](d) - ns_mean(d);
                    sum_sq += diff * diff;
                }
                ns_std(d) = std::sqrt(sum_sq / J + kEps);
            }
        }
        for (int j = 0; j < J; ++j) {
            for (int d = 0; d < kFingerprintDim; ++d) {
                msdfm_mapping_.native_fingerprints[j](d) =
                    (msdfm_mapping_.native_fingerprints[j](d) - ns_mean(d)) / ns_std(d);
            }
        }
        std::cerr << "[B2] cross-state std=[" << ns_std.transpose() << "]\n";
    }

    // Define target fingerprints for 6 ontology states (z-scored)
    //   [growth, inflation, credit_spread, yield_curve, policy_rate]
    //   Positive credit = stress, positive yield = normal curve, positive policy = tight
    msdfm_mapping_.target_fingerprints.resize(kNumMacroRegimes);
    Eigen::VectorXd t0(kFingerprintDim); t0 <<  1.5, -1.0, -1.0,  1.0, -0.5;  // EXP_DIS
    Eigen::VectorXd t1(kFingerprintDim); t1 <<  1.5,  1.0, -0.5,  0.0,  0.5;  // EXP_INF
    Eigen::VectorXd t2(kFingerprintDim); t2 <<  0.0, -1.0,  0.0,  0.0,  0.0;  // SLO_DIS
    Eigen::VectorXd t3(kFingerprintDim); t3 <<  0.0,  1.0,  0.5, -0.5,  1.0;  // SLO_INF
    Eigen::VectorXd t4(kFingerprintDim); t4 << -1.5, -1.5,  1.5,  1.5, -1.5;  // REC_DEF
    Eigen::VectorXd t5(kFingerprintDim); t5 << -1.5,  1.5,  1.5, -0.5,  1.5;  // REC_INF

    msdfm_mapping_.target_fingerprints[0] = t0;
    msdfm_mapping_.target_fingerprints[1] = t1;
    msdfm_mapping_.target_fingerprints[2] = t2;
    msdfm_mapping_.target_fingerprints[3] = t3;
    msdfm_mapping_.target_fingerprints[4] = t4;
    msdfm_mapping_.target_fingerprints[5] = t5;

    // Compute softmax distance mapping matrix (J x 6)
    msdfm_mapping_.tau = config_.fingerprint_tau;
    msdfm_mapping_.mapping_matrix = Eigen::MatrixXd::Zero(J, kNumMacroRegimes);

    for (int j = 0; j < J; ++j) {
        Eigen::VectorXd log_weights(kNumMacroRegimes);
        for (int k = 0; k < kNumMacroRegimes; ++k) {
            double dist = (msdfm_mapping_.native_fingerprints[j] -
                           msdfm_mapping_.target_fingerprints[k]).norm();
            log_weights(k) = -dist / msdfm_mapping_.tau;
        }
        // Softmax
        double max_lw = log_weights.maxCoeff();
        Eigen::VectorXd w = (log_weights.array() - max_lw).exp();
        w /= w.sum();
        msdfm_mapping_.mapping_matrix.row(j) = w.transpose();
    }

    std::cerr << "[B2] MS-DFM mapping matrix (" << J << "x" << kNumMacroRegimes << "):\n";
    for (int j = 0; j < J; ++j) {
        std::cerr << "  native " << j << ": [";
        for (int k = 0; k < kNumMacroRegimes; ++k) {
            if (k > 0) std::cerr << ", ";
            std::cerr << std::fixed << std::setprecision(3)
                      << msdfm_mapping_.mapping_matrix(j, k);
        }
        std::cerr << "]\n";
    }
}

// ============================================================================
// B2: MS-DFM Mapping (runtime)
// ============================================================================

Eigen::Matrix<double, kNumMacroRegimes, 1>
MacroRegimePipeline::map_msdfm(const Eigen::Vector3d& p_native) const {
    Eigen::Matrix<double, kNumMacroRegimes, 1> result =
        msdfm_mapping_.mapping_matrix.transpose() * p_native;

    // Normalise
    result = result.array().max(0.0);
    double s = result.sum();
    if (s > kEps) result /= s;
    else result.setConstant(1.0 / kNumMacroRegimes);

    return result;
}

// ============================================================================
// B3: Quadrant Mapping (runtime, rule-based)
// ============================================================================

Eigen::Matrix<double, kNumMacroRegimes, 1>
MacroRegimePipeline::map_quadrant(double growth_score, double inflation_score) const {
    double g_norm = (growth_score - growth_median_) / growth_std_;
    double i_norm = (inflation_score - inflation_median_) / inflation_std_;

    double band = config_.quadrant_blend_band;

    // Sigmoid weights for each axis
    // growth_high_weight: 1 = clearly high growth, 0 = clearly low
    double g_high = 0.5 * (1.0 + std::tanh(g_norm / band));
    double i_high = 0.5 * (1.0 + std::tanh(i_norm / band));
    double g_low  = 1.0 - g_high;
    double i_low  = 1.0 - i_high;

    // Quadrant weights (4 quadrants blend smoothly)
    double w_goldilocks = g_high * i_low;   // Q0: high growth, low inflation
    double w_reflation  = g_high * i_high;  // Q1: high growth, high inflation
    double w_deflation  = g_low  * i_low;   // Q2: low growth, low inflation
    double w_stagflation = g_low * i_high;  // Q3: low growth, high inflation

    Eigen::Matrix<double, kNumMacroRegimes, 1> result =
        w_goldilocks  * quadrant_table_.row(0).transpose() +
        w_reflation   * quadrant_table_.row(1).transpose() +
        w_deflation   * quadrant_table_.row(2).transpose() +
        w_stagflation * quadrant_table_.row(3).transpose();

    // Normalise (should sum to ~1 already)
    result /= result.sum();
    return result;
}

// ============================================================================
// B4: BSTS Training + Mapping
// ============================================================================

void MacroRegimePipeline::train_bsts_fingerprints(
    const Eigen::MatrixXd& bsts_probs,
    const MacroPanel& panel)
{
    const int T = std::min(static_cast<int>(bsts_probs.rows()), panel.T);
    const int K = static_cast<int>(bsts_probs.cols());

    // Compute fingerprints per BSTS cluster using hard assignments
    bsts_mapping_.native_fingerprints.resize(K);
    for (int k = 0; k < K; ++k) {
        std::vector<int> indices;
        for (int t = 0; t < T; ++t) {
            int best = 0;
            bsts_probs.row(t).maxCoeff(&best);
            if (best == k) indices.push_back(t);
        }
        // Already z-scored per column inside compute_fingerprint.
        Eigen::VectorXd fp = compute_fingerprint(panel, indices);
        bsts_mapping_.native_fingerprints[k] = fp;
    }

    // Stage 2: cross-cluster standardisation (same rationale as MS-DFM).
    {
        Eigen::VectorXd ns_mean = Eigen::VectorXd::Zero(kFingerprintDim);
        for (int k = 0; k < K; ++k) ns_mean += bsts_mapping_.native_fingerprints[k];
        if (K > 0) ns_mean /= K;

        Eigen::VectorXd ns_std = Eigen::VectorXd::Ones(kFingerprintDim);
        if (K >= 2) {
            for (int d = 0; d < kFingerprintDim; ++d) {
                double sum_sq = 0;
                for (int k = 0; k < K; ++k) {
                    double diff = bsts_mapping_.native_fingerprints[k](d) - ns_mean(d);
                    sum_sq += diff * diff;
                }
                ns_std(d) = std::sqrt(sum_sq / K + kEps);
            }
        }
        for (int k = 0; k < K; ++k) {
            for (int d = 0; d < kFingerprintDim; ++d) {
                bsts_mapping_.native_fingerprints[k](d) =
                    (bsts_mapping_.native_fingerprints[k](d) - ns_mean(d)) / ns_std(d);
            }
        }
    }

    // Reuse same target fingerprints as MS-DFM
    bsts_mapping_.target_fingerprints = msdfm_mapping_.target_fingerprints;
    bsts_mapping_.tau = config_.fingerprint_tau;

    // Build K x 6 mapping matrix
    bsts_mapping_.mapping_matrix = Eigen::MatrixXd::Zero(K, kNumMacroRegimes);
    for (int k = 0; k < K; ++k) {
        Eigen::VectorXd log_weights(kNumMacroRegimes);
        for (int r = 0; r < kNumMacroRegimes; ++r) {
            double dist = (bsts_mapping_.native_fingerprints[k] -
                           bsts_mapping_.target_fingerprints[r]).norm();
            log_weights(r) = -dist / bsts_mapping_.tau;
        }
        double max_lw = log_weights.maxCoeff();
        Eigen::VectorXd w = (log_weights.array() - max_lw).exp();
        w /= w.sum();
        bsts_mapping_.mapping_matrix.row(k) = w.transpose();
    }

    std::cerr << "[B4] BSTS mapping matrix (" << K << "x" << kNumMacroRegimes << ") trained\n";
}

Eigen::Matrix<double, kNumMacroRegimes, 1>
MacroRegimePipeline::map_bsts(const Eigen::Vector4d& cluster_probs,
                               bool structural_break) const {
    // Spec behaviour: no break → uniform (defer to other models).
    // Calibration override: if bsts_always_contribute is true, always use
    // the fingerprint mapping so BSTS's 10% weight is never wasted on
    // uninformative uniform distributions.
    if (!structural_break && !config_.bsts_always_contribute) {
        Eigen::Matrix<double, kNumMacroRegimes, 1> uniform;
        uniform.setConstant(1.0 / kNumMacroRegimes);
        return uniform;
    }

    // Fingerprint mapping (always during break, or always if bsts_always_contribute)
    Eigen::Matrix<double, kNumMacroRegimes, 1> result =
        bsts_mapping_.mapping_matrix.transpose() * cluster_probs;

    result = result.array().max(0.0);
    double s = result.sum();
    if (s > kEps) result /= s;
    else result.setConstant(1.0 / kNumMacroRegimes);

    return result;
}

// ============================================================================
// Aggregation
// ============================================================================

Eigen::Matrix<double, kNumMacroRegimes, 1>
MacroRegimePipeline::aggregate(
    const Eigen::Matrix<double, kNumMacroRegimes, 1>& p_dfm,
    const Eigen::Matrix<double, kNumMacroRegimes, 1>& p_msdfm,
    const Eigen::Matrix<double, kNumMacroRegimes, 1>& p_quad,
    const Eigen::Matrix<double, kNumMacroRegimes, 1>& p_bsts) const
{
    Eigen::Matrix<double, kNumMacroRegimes, 1> raw =
        config_.w_dfm      * p_dfm +
        config_.w_msdfm    * p_msdfm +
        config_.w_quadrant * p_quad +
        config_.w_bsts     * p_bsts;

    raw = raw.array().max(0.0);
    double s = raw.sum();
    if (s > kEps) raw /= s;
    else raw.setConstant(1.0 / kNumMacroRegimes);

    return raw;
}

// ============================================================================
// Persistence Smoothing
// ============================================================================

Eigen::Matrix<double, kNumMacroRegimes, 1>
MacroRegimePipeline::smooth(
    const Eigen::Matrix<double, kNumMacroRegimes, 1>& p_raw,
    bool shock_detected)
{
    // CALIBRATION NOTE: The spec starts prev_smoothed at uniform [1/6,...].
    // This biases the initial regime towards whichever bucket gets the
    // first above-uniform signal, then the heavy smoothing locks it in.
    // During the first 10 updates, we use lambda=1.0 (no smoothing) so the
    // pipeline can find its natural starting regime from the raw model
    // outputs before smoothing kicks in.
    constexpr int kWarmupSteps = 10;

    double lambda;
    if (update_count_ < kWarmupSteps) {
        lambda = 1.0;  // pure raw during warmup
    } else {
        double scale = shock_detected ? config_.shock_lambda_scale
                                       : config_.calm_lambda_scale;
        lambda = std::min(1.0, config_.base_lambda * scale);
    }

    Eigen::Matrix<double, kNumMacroRegimes, 1> result =
        lambda * p_raw + (1.0 - lambda) * prev_smoothed_;

    // Normalise
    result /= result.sum();
    prev_smoothed_ = result;

    return result;
}

// ============================================================================
// Hysteresis + Minimum Dwell
// ============================================================================

double MacroRegimePipeline::get_enter_threshold(MacroRegimeL1 r) const {
    int idx = static_cast<int>(r);
    // Expansion regimes = aggressive
    if (idx <= 1) return config_.enter_aggressive_thresh;
    // Slowdown regimes = defensive
    if (idx <= 3) return config_.enter_defensive_thresh;
    // Recession regimes = risk-off
    return config_.enter_riskoff_thresh;
}

double MacroRegimePipeline::get_exit_threshold(MacroRegimeL1 r) const {
    int idx = static_cast<int>(r);
    if (idx <= 1) return config_.exit_aggressive_thresh;
    if (idx <= 3) return config_.exit_defensive_thresh;
    return config_.exit_riskoff_thresh;
}

MacroRegimeL1 MacroRegimePipeline::apply_hysteresis(
    Eigen::Matrix<double, kNumMacroRegimes, 1>& p_smooth)
{
    // Find candidate (argmax)
    int candidate_idx;
    p_smooth.maxCoeff(&candidate_idx);
    auto candidate = static_cast<MacroRegimeL1>(candidate_idx);
    int current_idx = static_cast<int>(current_regime_);

    if (candidate == current_regime_) {
        dwell_counter_++;
        return current_regime_;
    }

    // Minimum dwell penalty
    if (dwell_counter_ < config_.min_dwell_bars) {
        double decay = 1.0 - static_cast<double>(dwell_counter_) / config_.min_dwell_bars;
        double bonus = config_.dwell_penalty * decay;
        p_smooth(current_idx) += bonus;
        p_smooth /= p_smooth.sum();

        // Re-check candidate
        p_smooth.maxCoeff(&candidate_idx);
        candidate = static_cast<MacroRegimeL1>(candidate_idx);
        if (candidate == current_regime_) {
            dwell_counter_++;
            return current_regime_;
        }
    }

    // Asymmetric threshold check
    // CALIBRATION NOTE: The spec uses absolute enter/exit thresholds, but
    // with 6 regimes and 4 disagreeing models, probabilities are spread thin.
    // The dominant regime often sits at 15-30%. An absolute exit threshold
    // can prevent transitions even when another regime has a clear lead.
    // Added a relative condition: if the candidate leads the current regime
    // by >5 percentage points, allow transition regardless of absolute
    // thresholds. This ensures the pipeline follows its own probability
    // signals rather than getting locked into stale regimes.
    double enter = get_enter_threshold(candidate);
    double exit_t = get_exit_threshold(current_regime_);
    double lead = p_smooth(candidate_idx) - p_smooth(current_idx);

    bool absolute_ok = p_smooth(candidate_idx) > enter && p_smooth(current_idx) < exit_t;
    bool relative_ok = lead > 0.05;  // candidate leads by >5pp

    if (absolute_ok || relative_ok) {
        current_regime_ = candidate;
        dwell_counter_ = 0;
        return candidate;
    }

    dwell_counter_++;
    return current_regime_;
}

// ============================================================================
// Confidence
// ============================================================================

double MacroRegimePipeline::compute_confidence(
    const Eigen::Matrix<double, kNumMacroRegimes, 1>& probs) const
{
    // Confidence = P(dominant) - P(second highest)
    Eigen::Matrix<double, kNumMacroRegimes, 1> sorted = probs;
    std::sort(sorted.data(), sorted.data() + kNumMacroRegimes, std::greater<double>());
    return sorted(0) - sorted(1);
}

// ============================================================================
// Update (runtime orchestration)
// ============================================================================

Result<MacroBelief> MacroRegimePipeline::update(
    const Eigen::Vector3d& dfm_factors,
    const Eigen::Vector3d& msdfm_native_probs,
    double growth_score,
    double inflation_score,
    const Eigen::Vector4d& bsts_cluster_probs,
    bool structural_break)
{
    if (!trained_) {
        return make_error<MacroBelief>(ErrorCode::NOT_INITIALIZED,
            "Pipeline not trained", "MacroRegimePipeline");
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // Map each model → 6 probs
    auto p_dfm   = map_dfm(dfm_factors);
    auto p_msdfm = map_msdfm(msdfm_native_probs);
    auto p_quad  = map_quadrant(growth_score, inflation_score);
    auto p_bsts  = map_bsts(bsts_cluster_probs, structural_break);

    // ── Failure degradation ──────────────────────────────────────────────
    // Detect model failure: output is all-NaN, all-zero, or contains NaN.
    // If a model fails, exclude it and redistribute its weight.
    auto is_valid = [](const Eigen::Matrix<double, kNumMacroRegimes, 1>& p) -> bool {
        for (int i = 0; i < kNumMacroRegimes; ++i)
            if (!std::isfinite(p(i))) return false;
        return p.sum() > 1e-10;
    };

    bool dfm_ok   = is_valid(p_dfm);
    bool msdfm_ok = is_valid(p_msdfm);
    bool quad_ok  = is_valid(p_quad);
    bool bsts_ok  = is_valid(p_bsts);

    double w_dfm_eff   = dfm_ok   ? config_.w_dfm      : 0.0;
    double w_msdfm_eff = msdfm_ok ? config_.w_msdfm    : 0.0;
    double w_quad_eff  = quad_ok  ? config_.w_quadrant : 0.0;
    double w_bsts_eff  = bsts_ok  ? config_.w_bsts     : 0.0;

    double w_total = w_dfm_eff + w_msdfm_eff + w_quad_eff + w_bsts_eff;

    if (w_total < 1e-10) {
        // All models failed — return uniform belief
        MacroBelief fallback;
        for (int r = 0; r < kNumMacroRegimes; ++r)
            fallback.macro_probs[static_cast<MacroRegimeL1>(r)] = 1.0 / kNumMacroRegimes;
        fallback.most_likely = current_regime_;
        fallback.confidence = 0.0;
        fallback.regime_age_bars = last_belief_.regime_age_bars + 1;
        fallback.timestamp = std::chrono::system_clock::now();
        last_belief_ = fallback;
        return fallback;
    }

    // Renormalise weights so active models sum to 1
    w_dfm_eff   /= w_total;
    w_msdfm_eff /= w_total;
    w_quad_eff  /= w_total;
    w_bsts_eff  /= w_total;

    if (!dfm_ok)   std::cerr << "[WARN] DFM model failed — excluded from aggregation\n";
    if (!msdfm_ok) std::cerr << "[WARN] MS-DFM model failed — excluded from aggregation\n";
    if (!quad_ok)  std::cerr << "[WARN] Quadrant model failed — excluded from aggregation\n";
    if (!bsts_ok)  std::cerr << "[WARN] BSTS model failed — excluded from aggregation\n";

    // Replace failed models with zero vectors for aggregation
    if (!dfm_ok)   p_dfm.setZero();
    if (!msdfm_ok) p_msdfm.setZero();
    if (!quad_ok)  p_quad.setZero();
    if (!bsts_ok)  p_bsts.setZero();

    // Aggregate with effective weights
    Eigen::Matrix<double, kNumMacroRegimes, 1> p_raw =
        w_dfm_eff   * p_dfm +
        w_msdfm_eff * p_msdfm +
        w_quad_eff  * p_quad +
        w_bsts_eff  * p_bsts;
    {
        p_raw = p_raw.array().max(0.0);
        double s = p_raw.sum();
        if (s > kEps) p_raw /= s;
        else p_raw.setConstant(1.0 / kNumMacroRegimes);
    }

    // Detect shock: large shift from previous smoothed
    bool shock = structural_break ||
                 (p_raw - prev_smoothed_).array().abs().maxCoeff() > config_.shock_threshold;

    // Smooth
    auto p_smooth = smooth(p_raw, shock);
    ++update_count_;

    // Hysteresis + dwell
    MacroRegimeL1 dominant = apply_hysteresis(p_smooth);

    // Confidence
    double confidence = compute_confidence(p_smooth);

    // Build MacroBelief
    MacroBelief belief;
    for (int r = 0; r < kNumMacroRegimes; ++r) {
        auto regime = static_cast<MacroRegimeL1>(r);
        belief.macro_probs[regime] = p_smooth(r);
    }
    belief.most_likely = dominant;
    belief.confidence = confidence;
    belief.structural_break_risk = structural_break;
    belief.timestamp = std::chrono::system_clock::now();

    if (dominant == last_belief_.most_likely) {
        belief.regime_age_bars = last_belief_.regime_age_bars + 1;
    } else {
        belief.regime_age_bars = 1;
    }

    // Store per-model contributions
    auto store_contrib = [](const std::string& name,
                            const Eigen::Matrix<double, kNumMacroRegimes, 1>& p)
        -> std::map<MacroRegimeL1, double> {
        std::map<MacroRegimeL1, double> m;
        for (int r = 0; r < kNumMacroRegimes; ++r)
            m[static_cast<MacroRegimeL1>(r)] = p(r);
        return m;
    };

    belief.model_contributions["DFM"]      = store_contrib("DFM", p_dfm);
    belief.model_contributions["MS-DFM"]   = store_contrib("MS-DFM", p_msdfm);
    belief.model_contributions["Quadrant"] = store_contrib("Quadrant", p_quad);
    belief.model_contributions["BSTS"]     = store_contrib("BSTS", p_bsts);

    last_belief_ = belief;
    return belief;
}

} // namespace statistics
} // namespace trade_ngin
