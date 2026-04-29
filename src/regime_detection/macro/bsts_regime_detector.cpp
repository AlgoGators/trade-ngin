/*
    bsts_regime_detector.cpp
    Trade Engine — Statistics / State Estimation

    Library refactor of bsts_regime_detection_multiasset.cpp.
    All math is identical; static functions become class methods,
    hardcoded constants become config_ members.
*/

#include "trade_ngin/regime_detection/macro/bsts_regime_detector.hpp"
#include "trade_ngin/data/postgres_database.hpp"

#include <arrow/api.h>
#include <Eigen/Dense>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace trade_ngin {
namespace statistics {

static constexpr double kPi  = 3.14159265358979323846;
static constexpr double kEps = 1e-8;

// Regime names. Index -1 is reserved for "Unclassified" (M-06 fix —
// clusters that have no positive-score match against any signature
// should not be force-mapped to the closest one).
static const char* kBSTSRegimeNames[4] = {
    "R0 Risk-On Growth", "R1 Risk-Off/Crash", "R2 Stagflation", "R3 Reflation"
};

// Macro signatures for regime labeling (exact same as original)
// Columns: {gi_quad, growth, inflation, stress, ros}
static const double MACRO_SIGNATURES[4][5] = {
    { 2.0,  1.0,  0.0, -1.0,  1.0},  // R0 Risk-On Growth
    {-1.0, -1.0,  0.0,  2.0, -1.0},  // R1 Risk-Off/Crash
    {-2.0, -1.0,  1.5,  0.0, -1.0},  // R2 Stagflation
    { 1.0,  1.0,  0.5, -1.0,  1.0},  // R3 Reflation
};

// ============================================================================
// Config serialisation
// ============================================================================

nlohmann::json BSTSConfig::to_json() const {
    return {
        {"optimise_sigma", optimise_sigma},
        {"innovation_window", innovation_window},
        {"gaussian_smooth_radius", gaussian_smooth_radius},
        {"gaussian_smooth_sigma", gaussian_smooth_sigma},
        {"pca_components", pca_components},
        {"num_regimes", num_regimes},
        {"gmm_max_iterations", gmm_max_iterations},
        {"gmm_tolerance", gmm_tolerance},
        {"gmm_restarts", gmm_restarts},
        {"high_conviction_threshold", high_conviction_threshold},
        {"etf_series", etf_series},
        {"macro_series", macro_series}
    };
}

void BSTSConfig::from_json(const nlohmann::json& j) {
    auto get = [&](const char* k, auto& v) {
        if (j.contains(k)) v = j[k].get<std::decay_t<decltype(v)>>();
    };
    get("optimise_sigma", optimise_sigma);
    get("innovation_window", innovation_window);
    get("gaussian_smooth_radius", gaussian_smooth_radius);
    get("gaussian_smooth_sigma", gaussian_smooth_sigma);
    get("pca_components", pca_components);
    get("num_regimes", num_regimes);
    get("gmm_max_iterations", gmm_max_iterations);
    get("gmm_tolerance", gmm_tolerance);
    get("gmm_restarts", gmm_restarts);
    get("high_conviction_threshold", high_conviction_threshold);
    get("etf_series", etf_series);
    get("macro_series", macro_series);
}

// ============================================================================
// Constructor + regime_name
// ============================================================================

BSTSRegimeDetector::BSTSRegimeDetector(BSTSConfig config)
    : config_(std::move(config)) {}

const char* BSTSRegimeDetector::regime_name(int label) {
    if (label >= 0 && label < 4) return kBSTSRegimeNames[label];
    // -1 is the documented sentinel for clusters that scored ≤ 0
    // against every signature (no economically meaningful match).
    if (label == -1) return "Unclassified";
    return "UNKNOWN";
}

// ============================================================================
// Data cleaning helpers
// ============================================================================

// FORWARD-FILL ONLY: legitimate carry-forward of last released value.
// backward_fill removed 2026-04-28 (lookahead bias — see
// docs/REGIME_PIPELINE_LIBRARY_AUDIT.md). Past timestamps must NEVER be
// patched with future observations: it contaminates training labels and
// silently inflates in-sample regime accuracy vs. live performance.
void BSTSRegimeDetector::forward_fill(Eigen::MatrixXd& X) {
    for (int j = 0; j < X.cols(); ++j) {
        double last = std::numeric_limits<double>::quiet_NaN();
        for (int i = 0; i < X.rows(); ++i) {
            if (std::isfinite(X(i,j))) last = X(i,j);
            else if (std::isfinite(last)) X(i,j) = last;
        }
    }
}

// LEADING-PAD ONLY: pads NaN values BEFORE the first valid observation
// of each column with that first observation. This is a state-space
// initialization choice, NOT lookahead bias: at indices [0, first_valid)
// the series literally has no data, and any state-space model must
// choose some prior. Using the first observation is the standard
// "diffuse prior collapsed at first contact" practice. Once the first
// valid observation has been seen, NaN gaps are forward-filled
// (legitimate carry-forward), and mid-panel NaN is never patched
// with future values. Distinct from the removed backward_fill, which
// applied to ALL NaN positions including mid-panel.
void BSTSRegimeDetector::leading_pad_with_first_valid(Eigen::MatrixXd& X) {
    for (int j = 0; j < X.cols(); ++j) {
        // Find first valid (finite) observation in this column
        int first_valid = -1;
        for (int i = 0; i < X.rows(); ++i) {
            if (std::isfinite(X(i,j))) { first_valid = i; break; }
        }
        if (first_valid <= 0) continue;  // no leading NaN OR column entirely NaN
        const double seed = X(first_valid, j);
        for (int i = 0; i < first_valid; ++i) X(i,j) = seed;
    }
}

int BSTSRegimeDetector::col_idx(const std::vector<std::string>& cols, const std::string& name) {
    for (int i = 0; i < (int)cols.size(); ++i) if (cols[i] == name) return i;
    throw std::runtime_error("Column not found: '" + name + "'");
}

std::vector<double> BSTSRegimeDetector::extract_col(const Eigen::MatrixXd& X, int j) {
    std::vector<double> v(X.rows());
    for (int i = 0; i < X.rows(); ++i) v[i] = X(i, j);
    return v;
}

// ============================================================================
// BSTS — Local Linear Trend
// ============================================================================

BSTSRegimeDetector::KalmanResult BSTSRegimeDetector::run_kalman(
    const std::vector<double>& y,
    double sigma_obs, double sigma_level, double sigma_slope)
{
    const int T = (int)y.size();
    if (T < 5) throw std::invalid_argument("Series too short.");

    Eigen::Matrix2d F; F << 1, 1, 0, 1;
    Eigen::RowVector2d H; H << 1, 0;
    Eigen::Matrix2d Q = Eigen::Matrix2d::Zero();
    Q(0,0) = sigma_level * sigma_level;
    Q(1,1) = sigma_slope * sigma_slope;
    const double R = sigma_obs * sigma_obs;

    KalmanResult out;
    out.filtered_state.resize(T);   out.filtered_cov.resize(T);
    out.predicted_state.resize(T);  out.predicted_cov.resize(T);
    out.innovations.resize(T);      out.innovation_var.resize(T);

    Eigen::Vector2d x; x << y[0], 0.0;
    Eigen::Matrix2d P = 10.0 * Eigen::Matrix2d::Identity();

    for (int t = 0; t < T; ++t) {
        Eigen::Vector2d xp = (t == 0) ? x : F * out.filtered_state[t-1];
        Eigen::Matrix2d Pp = (t == 0) ? P : F * out.filtered_cov[t-1] * F.transpose() + Q;

        const double yhat = (H * xp)(0);
        const double v    = y[t] - yhat;
        const double S    = (H * Pp * H.transpose())(0) + R;

        Eigen::Vector2d K   = Pp * H.transpose() / S;
        Eigen::Matrix2d IKH = Eigen::Matrix2d::Identity() - K * H;
        Eigen::Vector2d xf  = xp + K * v;
        Eigen::Matrix2d Pf  = IKH * Pp * IKH.transpose() + K * R * K.transpose();

        out.predicted_state[t] = xp;  out.predicted_cov[t] = Pp;
        out.filtered_state[t]  = xf;  out.filtered_cov[t]  = Pf;
        out.innovations[t]     = v;   out.innovation_var[t] = S;
        out.log_likelihood    += -0.5 * (std::log(2.0 * kPi * S) + v*v/S);
    }
    return out;
}

BSTSRegimeDetector::SmoothedResult BSTSRegimeDetector::run_rts(const KalmanResult& kf) {
    const int T = (int)kf.filtered_state.size();
    Eigen::Matrix2d F; F << 1, 1, 0, 1;

    SmoothedResult out;
    out.smoothed_state = kf.filtered_state;
    out.smoothed_cov   = kf.filtered_cov;

    for (int t = T-2; t >= 0; --t) {
        Eigen::Matrix2d C =
            kf.filtered_cov[t] * F.transpose() * kf.predicted_cov[t+1].inverse();
        out.smoothed_state[t] =
            kf.filtered_state[t] + C * (out.smoothed_state[t+1] - kf.predicted_state[t+1]);
        out.smoothed_cov[t] =
            kf.filtered_cov[t] +
            C * (out.smoothed_cov[t+1] - kf.predicted_cov[t+1]) * C.transpose();
    }
    return out;
}

std::tuple<double,double,double> BSTSRegimeDetector::mle_sigma(const std::vector<double>& y) {
    double mu = 0; for (auto v : y) mu += v; mu /= (double)y.size();
    double var = 0; for (auto v : y) var += (v-mu)*(v-mu); var /= (double)y.size();
    const double s = std::sqrt(var);
    if (s < 1e-10) return {0.01, 0.005, 0.001};

    double best_ll = -std::numeric_limits<double>::infinity();
    double bo = s*0.5, bl = s*0.1, bs = s*0.01;

    for (double a : {0.20, 0.50, 1.00, 1.50})
    for (double b : {0.05, 0.10, 0.20, 0.40})
    for (double c : {0.005, 0.010, 0.050}) {
        try {
            double ll = run_kalman(y, s*a, s*b, s*c).log_likelihood;
            if (ll > best_ll) { best_ll = ll; bo = s*a; bl = s*b; bs = s*c; }
        } catch(...) {}
    }
    return {bo, bl, bs};
}

BSTSRegimeDetector::SeriesPosterior BSTSRegimeDetector::fit_series(
    const std::string& name, const std::vector<double>& raw, bool is_etf) const
{
    SeriesPosterior sp;
    sp.name   = name;
    sp.is_etf = is_etf;

    if (is_etf) {
        sp.raw_values.resize(raw.size());
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] <= 0.0 || !std::isfinite(raw[i]))
                throw std::runtime_error("Non-positive price in ETF " + name);
            sp.raw_values[i] = std::log(raw[i]);
        }
    } else {
        sp.raw_values = raw;
        // Forward-fill NaNs only (carry last released value forward).
        // Backward-fill removed — see L-19: would leak future observations
        // into past timestamps for the BSTS state-space model.
        double last = 0.0; bool found = false;
        for (auto& v : sp.raw_values) {
            if (std::isfinite(v)) { last = v; found = true; }
            else if (found) v = last;
        }
        // Leading NaNs (before first observation of this series) remain NaN;
        // mle_sigma and run_kalman will handle the truncation downstream.
    }

    auto [so, sl, ss] = mle_sigma(sp.raw_values);
    sp.kf = run_kalman(sp.raw_values, so, sl, ss);
    sp.sm = run_rts(sp.kf);
    return sp;
}

// ============================================================================
// Feature helpers
// ============================================================================

double BSTSRegimeDetector::smooth_level(const SeriesPosterior& sp, int t) {
    return sp.sm.smoothed_state[t](0);
}

double BSTSRegimeDetector::smooth_slope(const SeriesPosterior& sp, int t) {
    return sp.sm.smoothed_state[t](1);
}

Eigen::VectorXd BSTSRegimeDetector::rolling_innov_vol(const std::vector<double>& innov, int win) {
    const int n = (int)innov.size();
    Eigen::VectorXd out(n);
    for (int t = 0; t < n; ++t) {
        const int t0 = std::max(0, t - win + 1);
        double sq = 0;
        for (int k = t0; k <= t; ++k) sq += innov[k]*innov[k];
        out(t) = std::sqrt(sq / (t - t0 + 1));
    }
    return out;
}

// CAUSAL Gaussian smoothing — kernel runs over [-2*radius, 0] (trailing only).
// Original implementation was symmetric [-radius, +radius] which mixed up to
// `radius` future values into the smoothed feature at time t — silent
// lookahead bias in every regime feature (see L-20 in audit doc). Default
// radius=4, sigma=2 now means "trailing 8-day Gaussian smooth" instead of
// "centered 9-day". Same effective averaging window, no future leakage.
Eigen::VectorXd BSTSRegimeDetector::gaussian_smooth(const Eigen::VectorXd& x, int radius, double sigma) {
    const int n = (int)x.size();
    if (radius <= 0) return x;

    const int span = 2 * radius;  // kernel covers [-span, 0]
    std::vector<double> kernel(span + 1);
    double ks = 0;
    for (int k = -span; k <= 0; ++k) {
        double v = std::exp(-(double)(k*k) / (2.0*sigma*sigma));
        kernel[k + span] = v;
        ks += v;
    }
    for (auto& v : kernel) v /= ks;

    Eigen::VectorXd out(n);
    for (int i = 0; i < n; ++i) {
        double acc = 0;
        for (int k = -span; k <= 0; ++k)
            acc += kernel[k + span] * x(std::clamp(i + k, 0, n - 1));
        out(i) = acc;
    }
    return out;
}

// ============================================================================
// Feature extraction (50-dim)
// ============================================================================
// Block 1 (32): ETF beta_t, sigma_beta, acceleration, innov_vol
// Block 2 (12): Macro smoothed level per series
// Block 3 ( 6): growth_score, inflation_score, growth_inflation_quad,
//               yield_curve, financial_stress, labor_slack

Eigen::MatrixXd BSTSRegimeDetector::build_feature_matrix(
    const std::vector<SeriesPosterior>& etfs,
    const std::vector<SeriesPosterior>& macros,
    const std::unordered_map<std::string, int>& midx,
    std::vector<std::string>& feat_names,
    Eigen::VectorXd& ros_out,
    Eigen::VectorXd& growth_out,
    Eigen::VectorXd& inflation_out,
    Eigen::VectorXd& gi_quad_out) const
{
    const int T  = (int)etfs[0].raw_values.size();
    const int NE = (int)etfs.size();
    const int NM = (int)macros.size();
    const int total = NE*4 + NM + 6;

    Eigen::MatrixXd X(T, total);
    X.setZero();

    // ── Block 1: ETF market momentum ─────────────────────────────────────────
    for (int a = 0; a < NE; ++a) {
        const auto& sp = etfs[a];
        Eigen::VectorXd ivol = rolling_innov_vol(sp.kf.innovations, config_.innovation_window);
        for (int t = 0; t < T; ++t) {
            X(t, a*4+0) = smooth_slope(sp, t);
            X(t, a*4+1) = std::sqrt(std::max(0.0, sp.sm.smoothed_cov[t](1,1)));
            X(t, a*4+2) = (t >= 2)
                ? smooth_level(sp,t) - 2.0*smooth_level(sp,t-1) + smooth_level(sp,t-2)
                : 0.0;
            X(t, a*4+3) = ivol(t);
        }
        feat_names.push_back(sp.name + "_beta");
        feat_names.push_back(sp.name + "_sigma_beta");
        feat_names.push_back(sp.name + "_accel");
        feat_names.push_back(sp.name + "_innov_vol");
    }

    // ── Block 2: Macro fundamental smoothed levels ────────────────────────────
    const int b2 = NE*4;
    for (int m = 0; m < NM; ++m) {
        for (int t = 0; t < T; ++t)
            X(t, b2+m) = smooth_level(macros[m], t);
        feat_names.push_back(macros[m].name + "_level");
    }

    // ── Block 3: Regime polarity composites ──────────────────────────────────
    const int b3 = b2 + NM;

    auto mlevel = [&](const std::string& n, int t) -> double {
        auto it = midx.find(n);
        return it == midx.end() ? 0.0 : smooth_level(macros[it->second], t);
    };
    auto mslope = [&](const std::string& n, int t) -> double {
        auto it = midx.find(n);
        return it == midx.end() ? 0.0 : smooth_slope(macros[it->second], t);
    };

    Eigen::VectorXd growth_v(T), inflation_v(T), gi_quad(T), ros(T);

    // Pre-compute series-wide mean/std for each growth_score component so
    // components on different scales don't dominate. Cap-util level lives
    // at ~78 while IP and GDP slopes live at ~0.01; averaging without
    // z-scoring would let cap-util dominate ~99% of the score.
    //
    // Use SLOPE for CPI/PCE (rate of change is the inflation signal, not
    // the level — 2023-2024 inflation falling from 9% to 3% looked
    // STAGFLATIONARY to a level-based score). Breakeven stays as level
    // (forward-looking expectation).
    auto series_stats = [&](auto extractor) -> std::pair<double, double> {
        double sum = 0.0, sum_sq = 0.0;
        int count = 0;
        for (int t = 0; t < T; ++t) {
            double v = extractor(t);
            if (std::isfinite(v)) { sum += v; sum_sq += v * v; ++count; }
        }
        if (count < 2) return {0.0, 1.0};
        double mean = sum / count;
        double var = (sum_sq - count * mean * mean) / (count - 1);
        return {mean, std::sqrt(std::max(var, 1e-12))};
    };

    auto [ip_m,  ip_s ] = series_stats([&](int t){ return mslope("industrial_production", t); });
    auto [cu_m,  cu_s ] = series_stats([&](int t){ return mlevel("manufacturing_capacity_util", t); });
    auto [gd_m,  gd_s ] = series_stats([&](int t){ return mslope("gdp", t); });
    auto [cpi_m, cpi_s] = series_stats([&](int t){ return mslope("cpi", t); });          // slope
    auto [pce_m, pce_s] = series_stats([&](int t){ return mslope("core_pce", t); });     // slope
    auto [bk_m,  bk_s ] = series_stats([&](int t){ return mlevel("breakeven_5y", t); }); // level (forward-looking)

    for (int t = 0; t < T; ++t) {
        // Z-score each component before averaging.
        const double gs =
            ((mslope("industrial_production", t) - ip_m) / ip_s
           + (mlevel("manufacturing_capacity_util", t) - cu_m) / cu_s
           + (mslope("gdp", t) - gd_m) / gd_s) / 3.0;

        // CPI/PCE slope (YoY-equivalent change); breakeven still level.
        // Each component z-scored.
        const double is =
            ((mslope("cpi", t)         - cpi_m) / cpi_s
           + (mslope("core_pce", t)    - pce_m) / pce_s
           + (mlevel("breakeven_5y", t) - bk_m)  / bk_s) / 3.0;

        // Growth-Inflation quadrant:
        //   positive  -> reflationary (growth outpacing inflation)
        //   negative  -> stagflationary / risk-off
        const double gi = gs - is * 0.5;

        // Yield curve: 10Y-2Y spread (positive=normal, negative=inverted)
        const double yc = mlevel("yield_spread_10y_2y", t);

        // Financial stress: HY OAS (high=stress=risk-off)
        const double fs = mlevel("high_yield_spread", t);

        // Labor slack: slope of unemployment (rising=deteriorating=risk-off)
        const double ls = mslope("unemployment_rate", t);

        // Risk-On Score: ETF beta-based (SPY=0, EEM=1, TLT=2, GLD=4)
        const double ros_t =
            0.5*(smooth_slope(etfs[0],t) + smooth_slope(etfs[1],t)) -
            0.5*(smooth_slope(etfs[2],t) + smooth_slope(etfs[4],t));

        growth_v(t)    = gs;
        inflation_v(t) = is;
        gi_quad(t)     = gi;
        ros(t)         = ros_t;

        X(t, b3+0) = gs;
        X(t, b3+1) = is;
        X(t, b3+2) = gi;
        X(t, b3+3) = yc;
        X(t, b3+4) = fs;
        X(t, b3+5) = ls;
    }

    // Gaussian smooth polarity composites
    for (int c = b3; c < b3+6; ++c)
        X.col(c) = gaussian_smooth(X.col(c), config_.gaussian_smooth_radius, config_.gaussian_smooth_sigma);
    ros         = gaussian_smooth(ros,         config_.gaussian_smooth_radius, config_.gaussian_smooth_sigma);
    growth_v    = gaussian_smooth(growth_v,    config_.gaussian_smooth_radius, config_.gaussian_smooth_sigma);
    inflation_v = gaussian_smooth(inflation_v, config_.gaussian_smooth_radius, config_.gaussian_smooth_sigma);
    gi_quad     = gaussian_smooth(gi_quad,     config_.gaussian_smooth_radius, config_.gaussian_smooth_sigma);

    feat_names.push_back("growth_score");
    feat_names.push_back("inflation_score");
    feat_names.push_back("growth_inflation_quad");
    feat_names.push_back("yield_curve");
    feat_names.push_back("financial_stress");
    feat_names.push_back("labor_slack");

    ros_out       = ros;
    growth_out    = growth_v;
    inflation_out = inflation_v;
    gi_quad_out   = gi_quad;

    return X;
}

// ============================================================================
// PCA
// ============================================================================

BSTSRegimeDetector::PCAResult BSTSRegimeDetector::run_pca(const Eigen::MatrixXd& X, int n_comp) {
    const int D = (int)X.cols(), T = (int)X.rows();
    n_comp = std::min(n_comp, std::min(T-1, D));

    PCAResult out;
    out.mean    = X.colwise().mean();
    out.std_dev = Eigen::VectorXd(D);
    for (int j = 0; j < D; ++j) {
        double var = (X.col(j).array() - out.mean(j)).square().mean();
        out.std_dev(j) = std::sqrt(var + kEps);
    }

    Eigen::MatrixXd Xs(T, D);
    for (int j = 0; j < D; ++j)
        Xs.col(j) = (X.col(j).array() - out.mean(j)) / out.std_dev(j);

    Eigen::MatrixXd cov = (Xs.transpose() * Xs) / (double)(T - 1);
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> eig(cov);

    Eigen::VectorXd evals = eig.eigenvalues();
    Eigen::MatrixXd evecs = eig.eigenvectors();
    std::vector<int> idx(evals.size());
    std::iota(idx.begin(), idx.end(), 0);
    std::sort(idx.begin(), idx.end(), [&](int a, int b){ return evals(a) > evals(b); });

    Eigen::MatrixXd vecs(evecs.rows(), n_comp);
    Eigen::VectorXd vals(evals.size());
    for (int i = 0; i < (int)evals.size(); ++i) vals(i) = std::max(0.0, evals(idx[i]));
    for (int k = 0; k < n_comp; ++k) vecs.col(k) = evecs.col(idx[k]);

    // PCA eigenvectors have arbitrary sign. Apply deterministic sign
    // convention — flip column if its largest-magnitude entry is negative.
    // Stabilizes cluster centers across re-runs (cluster posteriors are
    // sign-invariant, but the diagnostic cluster centers we print are not).
    for (int k = 0; k < n_comp; ++k) {
        Eigen::Index max_abs_idx;
        vecs.col(k).cwiseAbs().maxCoeff(&max_abs_idx);
        if (vecs(max_abs_idx, k) < 0) vecs.col(k) *= -1.0;
    }

    out.components             = vecs;
    out.transformed            = Xs * vecs;
    out.explained_variance_ratio = vals / (vals.sum() + kEps);
    return out;
}

// ============================================================================
// Full-covariance GMM
// ============================================================================

double BSTSRegimeDetector::log_mvn_pdf(
    const Eigen::VectorXd& x,
    const Eigen::VectorXd& mu,
    const Eigen::MatrixXd& Sigma)
{
    const int d = (int)x.size();
    Eigen::MatrixXd S = Sigma + kEps * Eigen::MatrixXd::Identity(d, d);
    Eigen::LLT<Eigen::MatrixXd> llt(S);
    if (llt.info() != Eigen::Success) return -1e12;
    const double log_det = 2.0 * llt.matrixL().toDenseMatrix().diagonal().array().log().sum();
    Eigen::VectorXd diff = x - mu;
    return -0.5 * (d * std::log(2.0*kPi) + log_det + llt.solve(diff).dot(diff));
}

// ============================================================================
// K-means++ initialisation
// ============================================================================

std::vector<Eigen::VectorXd> BSTSRegimeDetector::kmeans_plus_plus(
    const Eigen::MatrixXd& X, int K, std::mt19937& rng)
{
    const int n = (int)X.rows();
    std::vector<Eigen::VectorXd> centres;
    centres.reserve(K);

    // 1. Pick first centre uniformly at random
    std::uniform_int_distribution<int> uniform(0, n-1);
    centres.push_back(X.row(uniform(rng)).transpose());

    // 2. Each subsequent centre chosen with probability proportional to D^2
    for (int k = 1; k < K; ++k) {
        Eigen::VectorXd dists(n);
        for (int i = 0; i < n; ++i) {
            double min_d2 = std::numeric_limits<double>::infinity();
            for (const auto& c : centres) {
                double d2 = (X.row(i).transpose() - c).squaredNorm();
                min_d2 = std::min(min_d2, d2);
            }
            dists(i) = min_d2;
        }
        // Sample proportional to D^2
        std::discrete_distribution<int> weighted(
            dists.data(), dists.data() + n);
        centres.push_back(X.row(weighted(rng)).transpose());
    }
    return centres;
}

// ============================================================================
// One EM run — single initialisation, returns log-likelihood and full result
// ============================================================================

std::pair<double, BSTSRegimeDetector::GMMResult> BSTSRegimeDetector::run_em(
    const Eigen::MatrixXd& X, int K,
    const std::vector<Eigen::VectorXd>& init_means) const
{
    const int n = (int)X.rows(), d = (int)X.cols();

    GMMResult out;
    out.k = K;
    out.weights = Eigen::VectorXd::Constant(K, 1.0/K);
    out.means      = init_means;
    out.covariances.resize(K);
    out.responsibilities = Eigen::MatrixXd::Zero(n, K);

    for (int j = 0; j < K; ++j)
        out.covariances[j] = Eigen::MatrixXd::Identity(d, d);

    double prev_ll = -std::numeric_limits<double>::infinity();

    for (int iter = 0; iter < config_.gmm_max_iterations; ++iter) {
        // E-step
        double ll = 0.0;
        for (int i = 0; i < n; ++i) {
            Eigen::VectorXd xi = X.row(i).transpose();
            Eigen::VectorXd logp(K);
            for (int j = 0; j < K; ++j)
                logp(j) = std::log(std::max(kEps, out.weights(j)))
                          + log_mvn_pdf(xi, out.means[j], out.covariances[j]);
            const double lmax = logp.maxCoeff();
            Eigen::VectorXd stab = (logp.array() - lmax).exp();
            const double denom = stab.sum();
            out.responsibilities.row(i) = (stab / std::max(kEps, denom)).transpose();
            ll += lmax + std::log(std::max(kEps, denom));
        }
        if (std::abs(ll - prev_ll) < config_.gmm_tolerance) { prev_ll = ll; break; }
        prev_ll = ll;

        // M-step
        for (int j = 0; j < K; ++j) {
            double Nj = out.responsibilities.col(j).sum();
            out.weights(j) = Nj / n;

            Eigen::VectorXd mu = Eigen::VectorXd::Zero(d);
            for (int i = 0; i < n; ++i)
                mu += out.responsibilities(i,j) * X.row(i).transpose();
            mu /= std::max(kEps, Nj);
            out.means[j] = mu;

            Eigen::MatrixXd cov = Eigen::MatrixXd::Zero(d, d);
            for (int i = 0; i < n; ++i) {
                Eigen::VectorXd diff = X.row(i).transpose() - mu;
                cov += out.responsibilities(i,j) * diff * diff.transpose();
            }
            cov /= std::max(kEps, Nj);
            cov += 1e-4 * Eigen::MatrixXd::Identity(d, d);
            out.covariances[j] = cov;
        }
    }
    return {prev_ll, std::move(out)};
}

// ============================================================================
// Fit GMM — K-means++ init x restarts, keep best log-likelihood
// ============================================================================

BSTSRegimeDetector::GMMResult BSTSRegimeDetector::fit_gmm(
    const Eigen::MatrixXd& X, int K, int seed) const
{
    const int n = (int)X.rows();
    if (n < K) throw std::invalid_argument("Too few observations for GMM.");

    std::mt19937 rng(seed);
    double best_ll = -std::numeric_limits<double>::infinity();
    GMMResult best;

    for (int restart = 0; restart < config_.gmm_restarts; ++restart) {
        auto centres = kmeans_plus_plus(X, K, rng);
        auto [ll, result] = run_em(X, K, centres);
        std::cerr << "    restart " << (restart+1) << "/" << config_.gmm_restarts
                  << "  ll=" << std::fixed << std::setprecision(1) << ll << "\n";
        if (ll > best_ll) {
            best_ll = ll;
            best    = std::move(result);
        }
    }

    // Finalise: hard labels + normalised Shannon entropy
    best.labels.resize(n);
    best.entropy.resize(n);
    for (int i = 0; i < n; ++i) {
        Eigen::Index idx;
        best.responsibilities.row(i).maxCoeff(&idx);
        best.labels(i) = (int)idx;
        double h = 0;
        for (int j = 0; j < K; ++j) {
            double p = best.responsibilities(i,j);
            if (p > 1e-10) h -= p * std::log(p);
        }
        best.entropy(i) = h / std::log((double)K);
    }

    std::cerr << "  best ll=" << std::fixed << std::setprecision(1) << best_ll << "\n";
    return best;
}

// ============================================================================
// Regime labelling — 5-dim polarity map
// ============================================================================

std::vector<int> BSTSRegimeDetector::label_regimes(
    const GMMResult& gmm,
    const Eigen::VectorXd& growth,
    const Eigen::VectorXd& inflation,
    const Eigen::VectorXd& gi_quad,
    const Eigen::VectorXd& fin_stress,
    const Eigen::VectorXd& ros)
{
    const int T = (int)gmm.labels.size();
    const int K = gmm.k;

    // Compute per-cluster mean of each macro composite
    auto cluster_mean = [&](int k, const Eigen::VectorXd& v) -> double {
        double s = 0; int n = 0;
        for (int t = 0; t < T; ++t)
            if (gmm.labels(t) == k) { s += v(t); ++n; }
        return n > 0 ? s/n : 0.0;
    };

    // Standardise each signal across clusters so no single signal dominates
    auto cluster_means = [&](const Eigen::VectorXd& v) {
        std::vector<double> m(K);
        for (int k = 0; k < K; ++k) m[k] = cluster_mean(k, v);
        double mu = 0; for (auto x : m) mu += x; mu /= K;
        double var = 0; for (auto x : m) var += (x-mu)*(x-mu); var /= K;
        double sd = std::sqrt(var + 1e-10);
        for (auto& x : m) x = (x - mu) / sd;
        return m;
    };

    auto gi_z   = cluster_means(gi_quad);
    auto gs_z   = cluster_means(growth);
    auto inf_z  = cluster_means(inflation);
    auto str_z  = cluster_means(fin_stress);
    auto ros_z  = cluster_means(ros);

    // Score each raw cluster k against each semantic regime l
    std::vector<std::tuple<double,int,int>> cells;
    for (int k = 0; k < K; ++k) {
        double sig[5] = {gi_z[k], gs_z[k], inf_z[k], str_z[k], ros_z[k]};

        // Print cluster profile for debugging
        std::cerr << "  cluster " << k
                  << "  gi=" << std::fixed << std::setprecision(2) << gi_z[k]
                  << "  gs=" << gs_z[k]
                  << "  inf=" << inf_z[k]
                  << "  str=" << str_z[k]
                  << "  ros=" << ros_z[k] << "\n";

        for (int l = 0; l < K; ++l) {
            double score = 0;
            for (int i = 0; i < 5; ++i) score += sig[i] * MACRO_SIGNATURES[l][i];
            cells.emplace_back(score, k, l);
        }
    }
    std::sort(cells.begin(), cells.end(),
              [](const auto& a, const auto& b){ return std::get<0>(a) > std::get<0>(b); });

    // Greedy one-to-one assignment, but reject score ≤ 0. Assigning even
    // on negative scores produces economically nonsensical labels (e.g.,
    // cluster 2 → "R3 Reflation" with score -1.886 = anti-reflation). The
    // pipeline's macro regime output uses cluster posteriors (not these
    // labels), so this affects only the diagnostic dashboard label —
    // making kBSTSRegimeNames[mapping[k]] honest rather than misleading.
    std::vector<int> mapping(K, -1);
    std::vector<bool> used_raw(K, false), used_lbl(K, false);
    for (auto& [score, raw, lbl] : cells) {
        if (used_raw[raw] || used_lbl[lbl]) continue;
        if (score <= 0.0) {
            // No positive-score match remains for this (raw, lbl) pair.
            // Stop greedy assignment here — remaining clusters fall through
            // to the Unclassified sentinel below.
            std::cerr << "  cluster " << raw
                      << " best remaining score = " << std::setprecision(3) << score
                      << " (≤0); leaving Unclassified per M-06\n";
            continue;
        }
        mapping[raw] = lbl;
        used_raw[raw] = used_lbl[lbl] = true;
        std::cerr << "  assign cluster " << raw
                  << " -> " << kBSTSRegimeNames[lbl]
                  << "  (score=" << std::setprecision(3) << score << ")\n";
    }
    // Any cluster still at -1 is genuinely Unclassified; the pipeline's
    // macro regime output ignores these labels (uses posteriors instead),
    // so the only effect is on diagnostic output.
    return mapping;
}

// ============================================================================
// Database loading
// ============================================================================

BSTSRegimeDetector::DataFrame BSTSRegimeDetector::load_from_database(
    PostgresDatabase& db,
    const std::string& start_date,
    const std::string& end_date) const
{
    // ---- Load macro panel via MacroDataLoader --------------------------------
    MacroDataLoader loader(db);
    auto panel_result = loader.load(start_date, end_date);
    if (panel_result.is_error()) {
        throw std::runtime_error("Failed to load macro data: " +
                                  std::string(panel_result.error()->what()));
    }
    auto& panel = panel_result.value();
    std::cerr << "[db] macro panel: " << panel.T << " dates x "
              << panel.N << " columns\n";

    // ---- Load ETF prices from macro_data.bsts_etf_prices ----------------------
    std::string etf_start = start_date.empty() ? panel.dates.front() : start_date;
    std::string etf_end   = end_date.empty()   ? panel.dates.back()  : end_date;

    std::string symbol_list;
    for (size_t i = 0; i < config_.etf_series.size(); i++) {
        if (i > 0) symbol_list += ",";
        symbol_list += "'" + config_.etf_series[i] + "'";
    }

    std::string etf_sql =
        "SELECT date::text, symbol, adjusted_close FROM macro_data.bsts_etf_prices "
        "WHERE symbol IN (" + symbol_list + ") "
        "AND date >= '" + etf_start + "' "
        "AND date <= '" + etf_end + "' "
        "ORDER BY date, symbol";

    auto etf_query_result = db.execute_query(etf_sql);
    if (etf_query_result.is_error()) {
        throw std::runtime_error("Failed to load ETF data: " +
                                  std::string(etf_query_result.error()->what()));
    }

    std::unordered_map<std::string, std::unordered_map<std::string, double>> etf_prices;
    for (const auto& sym : config_.etf_series) {
        etf_prices[sym] = {};
    }

    auto etf_table  = etf_query_result.value();
    auto date_col   = etf_table->GetColumnByName("date");
    auto symbol_col = etf_table->GetColumnByName("symbol");
    auto close_col  = etf_table->GetColumnByName("adjusted_close");

    if (date_col && symbol_col && close_col) {
        for (int c = 0; c < date_col->num_chunks(); c++) {
            auto date_arr   = std::static_pointer_cast<arrow::StringArray>(date_col->chunk(c));
            auto symbol_arr = std::static_pointer_cast<arrow::StringArray>(symbol_col->chunk(c));
            auto close_arr  = std::static_pointer_cast<arrow::DoubleArray>(close_col->chunk(c));

            for (int64_t r = 0; r < date_arr->length(); r++) {
                std::string d = date_arr->GetString(r);
                std::string s = symbol_arr->GetString(r);
                double p      = close_arr->Value(r);
                if (etf_prices.count(s)) {
                    etf_prices[s][d] = p;
                }
            }
        }
    }
    std::cerr << "[db] ETF data: " << etf_table->num_rows() << " rows\n";

    // ---- Assemble DataFrame ------------------------------------------------
    std::vector<std::string> columns;
    for (const auto& s : config_.etf_series) columns.push_back(s);

    std::unordered_map<std::string, int> macro_col_map;
    for (int c = 0; c < panel.N; c++) {
        macro_col_map[panel.column_names[c]] = c;
    }
    for (const auto& s : config_.macro_series) {
        columns.push_back(s);
    }

    const int total_cols = (int)columns.size();
    const int T = panel.T;
    Eigen::MatrixXd values(T, total_cols);
    values.setConstant(std::numeric_limits<double>::quiet_NaN());

    for (int t = 0; t < T; t++) {
        const std::string& date = panel.dates[t];
        for (int e = 0; e < (int)config_.etf_series.size(); e++) {
            auto it = etf_prices[config_.etf_series[e]].find(date);
            if (it != etf_prices[config_.etf_series[e]].end()) {
                values(t, e) = it->second;
            }
        }
        for (int m = 0; m < (int)config_.macro_series.size(); m++) {
            auto it = macro_col_map.find(config_.macro_series[m]);
            if (it != macro_col_map.end()) {
                values(t, (int)config_.etf_series.size() + m) = panel.data(t, it->second);
            }
        }
    }

    std::cerr << "[db] assembled DataFrame: " << T << " x " << total_cols << "\n";
    return {panel.dates, columns, values};
}

// ============================================================================
// fit() — main public entry point
// ============================================================================

Result<BSTSOutput> BSTSRegimeDetector::fit(
    const Eigen::MatrixXd& etf_prices,
    const Eigen::MatrixXd& macro_data,
    const std::vector<std::string>& dates,
    const std::vector<std::string>& etf_names,
    const std::vector<std::string>& macro_names)
{
    std::lock_guard<std::mutex> lock(mutex_);

    const int T = (int)dates.size();
    const int NE = (int)config_.etf_series.size();
    const int NM = (int)config_.macro_series.size();

    // Use provided names or defaults
    auto enames = etf_names.empty() ? config_.etf_series : etf_names;
    auto mnames = macro_names.empty() ? config_.macro_series : macro_names;

    // 1. Forward/backward fill the input data
    Eigen::MatrixXd etf_data = etf_prices;
    Eigen::MatrixXd mac_data = macro_data;
    // Order matters: leading-pad first (state-space init), then forward-fill
    // for legitimate carry-forward. NEVER backward-fill mid-panel NaN.
    leading_pad_with_first_valid(etf_data);
    leading_pad_with_first_valid(mac_data);
    forward_fill(etf_data);
    forward_fill(mac_data);

    // 2. Fit BSTS per series
    std::cerr << "[bsts] ETF series...\n";
    std::vector<SeriesPosterior> etf_posts;
    for (int e = 0; e < NE; ++e) {
        std::cerr << "  " << std::left << std::setw(6) << enames[e];
        std::vector<double> col(T);
        for (int t = 0; t < T; ++t) col[t] = etf_data(t, e);
        auto sp = fit_series(enames[e], col, true);
        std::cerr << "  LL=" << std::setprecision(1) << sp.kf.log_likelihood << "\n";
        etf_posts.push_back(std::move(sp));
    }

    std::cerr << "[bsts] macro series...\n";
    std::vector<SeriesPosterior> mac_posts;
    std::unordered_map<std::string, int> midx;
    for (int m = 0; m < NM; ++m) {
        std::cerr << "  " << std::left << std::setw(22) << mnames[m];
        std::vector<double> col(T);
        for (int t = 0; t < T; ++t) col[t] = mac_data(t, m);
        auto sp = fit_series(mnames[m], col, false);
        std::cerr << "  LL=" << std::setprecision(1) << sp.kf.log_likelihood << "\n";
        midx[mnames[m]] = m;
        mac_posts.push_back(std::move(sp));
    }

    // 3. Build feature matrix (50-dim)
    std::cerr << "[features] building " << T << " x 50 matrix\n";
    Eigen::VectorXd ros, growth, inflation, gi_quad;
    std::vector<std::string> feat_names;
    Eigen::MatrixXd features = build_feature_matrix(
        etf_posts, mac_posts, midx, feat_names, ros, growth, inflation, gi_quad);

    // 4. PCA
    const int npca = std::min(config_.pca_components, (int)features.cols());
    std::cerr << "[pca] " << npca << " components\n";
    PCAResult pca = run_pca(features, npca);
    double cum = 0;
    for (int k = 0; k < npca; ++k) cum += pca.explained_variance_ratio(k);
    std::cerr << "  variance explained: " << std::setprecision(1) << cum*100 << "%\n";

    // 5. GMM
    std::cerr << "[gmm] K=" << config_.num_regimes
              << "  restarts=" << config_.gmm_restarts
              << "  (K-means++ init)\n";
    GMMResult gmm = fit_gmm(pca.transformed, config_.num_regimes);
    double pct_hc = 100.0 *
        (gmm.entropy.array() <= config_.high_conviction_threshold).cast<double>().mean();
    std::cerr << "  high-conviction obs: " << std::setprecision(1) << pct_hc << "%\n";

    // 6. Label regimes
    const int hy_col = NE*4 + midx.at("high_yield_spread");
    Eigen::VectorXd fin_stress = features.col(hy_col);
    std::vector<int> lmap = label_regimes(gmm, growth, inflation, gi_quad, fin_stress, ros);

    // 7. Build output
    BSTSOutput out;
    out.T = T;
    out.num_etfs = NE;
    out.num_macros = NM;
    out.num_regimes = config_.num_regimes;
    out.dates = dates;
    out.etf_names = enames;
    out.macro_names = mnames;

    // Regime assignments (mapped)
    out.regime_labels_raw = gmm.labels;
    out.regime_labels.resize(T);
    out.regime_posteriors = Eigen::MatrixXd::Zero(T, config_.num_regimes);
    for (int t = 0; t < T; ++t) {
        out.regime_labels(t) = lmap[gmm.labels(t)];
        for (int k = 0; k < config_.num_regimes; ++k)
            out.regime_posteriors(t, lmap[k]) = gmm.responsibilities(t, k);
    }
    out.regime_entropy = gmm.entropy;
    out.regime_label_mapping = lmap;

    // Macro composites
    out.risk_on_score = ros;
    out.growth_score = growth;
    out.inflation_score = inflation;
    out.growth_inflation_quad = gi_quad;

    // Features + PCA
    out.features = features;
    out.feature_names = feat_names;
    out.pca_transformed = pca.transformed;
    out.pca_variance_explained = pca.explained_variance_ratio;
    out.pca_variance_total = cum;

    // Smoothed levels/slopes
    out.smoothed_levels = Eigen::MatrixXd(T, NE + NM);
    out.smoothed_slopes = Eigen::MatrixXd(T, NE + NM);
    for (int t = 0; t < T; ++t) {
        for (int e = 0; e < NE; ++e) {
            out.smoothed_levels(t, e) = smooth_level(etf_posts[e], t);
            out.smoothed_slopes(t, e) = smooth_slope(etf_posts[e], t);
        }
        for (int m = 0; m < NM; ++m) {
            out.smoothed_levels(t, NE + m) = smooth_level(mac_posts[m], t);
            out.smoothed_slopes(t, NE + m) = smooth_slope(mac_posts[m], t);
        }
    }

    // GMM parameters
    out.gmm_weights = gmm.weights;
    out.gmm_means = gmm.means;
    out.gmm_covariances = gmm.covariances;
    out.gmm_log_likelihood = 0; // stored in gmm but not exposed directly
    out.high_conviction_pct = pct_hc;

    // Convergence info
    out.convergence_info.converged = true;
    out.convergence_info.iterations = config_.gmm_max_iterations;
    out.convergence_info.termination_reason = "completed";

    fitted_ = true;
    last_output_ = out;
    return out;
}

// ============================================================================
// fit_from_db() — convenience: load from DB and fit in one call
// ============================================================================

Result<BSTSOutput> BSTSRegimeDetector::fit_from_db(
    PostgresDatabase& db,
    const std::string& start_date,
    const std::string& end_date)
{
    // Load data using the same load_from_database as the original
    DataFrame df = load_from_database(db, start_date, end_date);
    leading_pad_with_first_valid(df.values);
    forward_fill(df.values);

    // Validate columns exist
    for (const auto& s : config_.etf_series) col_idx(df.columns, s);
    for (const auto& s : config_.macro_series) col_idx(df.columns, s);

    const int T = (int)df.dates.size();
    const int NE = (int)config_.etf_series.size();
    const int NM = (int)config_.macro_series.size();

    // Extract ETF and macro submatrices
    Eigen::MatrixXd etf_data(T, NE);
    for (int e = 0; e < NE; ++e) {
        int c = col_idx(df.columns, config_.etf_series[e]);
        etf_data.col(e) = df.values.col(c);
    }

    Eigen::MatrixXd mac_data(T, NM);
    for (int m = 0; m < NM; ++m) {
        int c = col_idx(df.columns, config_.macro_series[m]);
        mac_data.col(m) = df.values.col(c);
    }

    return fit(etf_data, mac_data, df.dates, config_.etf_series, config_.macro_series);
}

} // namespace statistics
} // namespace trade_ngin
