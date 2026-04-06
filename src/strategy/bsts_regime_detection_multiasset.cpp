/*
    bsts_regime_detection_multiasset.cpp
    AlgoGators Investment Fund — Quantitative Research

    Multi-asset BSTS macro regime detector.
    Reads weekly prices + macro fundamentals from CSV,
    runs LLT BSTS per series (Kalman + RTS smoother),
    extracts 50-dim posterior feature matrix (3 blocks),
    PCA to 12 components, GMM regime classification.

    Feature blocks:
      Block 1 — Market momentum  (8 ETFs × 4 features = 32)
                β_t, σ_β, ∇²μ, innov_vol per ETF
      Block 2 — Macro levels     (12 FRED series × 1 feature = 12)
                smoothed BSTS level μ_t per fundamental series
      Block 3 — Regime polarity  (6 cross-series composites)
                growth_score, inflation_score, growth_inflation_quad,
                yield_curve, financial_stress, labor_slack
      Total = 50 features → PCA 12 → GMM 4

    Compile (CMake inside trade-ngin):
        cmake --build build --config Release --target regime_detector

    Compile standalone:
        g++ -O2 -std=c++17 bsts_regime_detection_multiasset.cpp \
            -I /path/to/eigen3 -o regime_detector

    Run:
        ./regime_detector assets/weekly_macro_prices.csv assets/regime_output.csv

    Input CSV (produced by download_weekly_macro_data.py):
        date,SPY,EEM,TLT,HYG,GLD,UUP,USO,CPER,
             cpi_yoy,pce_yoy,breakeven_10y,
             gdp_qoq,cfnai,indpro_yoy,
             t10y2y,real_rate_10y,hy_oas,
             unrate,payrolls_mom,init_claims
*/

#include <Eigen/Dense>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

static constexpr double kPi             = 3.14159265358979323846;
static constexpr double kEps            = 1e-8;
static constexpr int    kInnovWin       = 8;
static constexpr int    kPCAComponents  = 12;
static constexpr int    kGMMRegimes     = 4;
static constexpr int    kGMMIter        = 300;
static constexpr double kGMMTol         = 1e-5;
static constexpr int    kGMMRestarts    = 10;  // K-means++ random restarts
static constexpr double kHighConviction = 0.20;

// ─────────────────────────────────────────────────────────────────────────────
// SERIES DEFINITIONS
// ─────────────────────────────────────────────────────────────────────────────

static const std::vector<std::string> ETF_SERIES = {
    "SPY", "EEM", "TLT", "HYG", "GLD", "UUP", "USO", "CPER"
};

// FRED macro series — model the raw transformed value directly (not log)
static const std::vector<std::string> MACRO_SERIES = {
    "cpi_yoy",       // CPI YoY %
    "pce_yoy",       // PCE YoY %
    "breakeven_10y", // 10Y inflation breakeven
    "gdp_qoq",       // GDP QoQ annualised %
    "cfnai",          // Chicago Fed NAI (>0=above-trend growth)
    "indpro_yoy",    // Industrial production YoY %
    "t10y2y",        // 10Y-2Y yield spread
    "real_rate_10y", // 10Y TIPS real yield
    "hy_oas",        // HY OAS credit spread
    "unrate",        // Unemployment rate
    "payrolls_mom",  // Nonfarm payrolls MoM %
    "init_claims",   // Initial jobless claims
};

// ─────────────────────────────────────────────────────────────────────────────
// DATA STRUCTURES
// ─────────────────────────────────────────────────────────────────────────────

struct DataFrame {
    std::vector<std::string> dates;
    std::vector<std::string> columns;
    Eigen::MatrixXd          values;
};

struct KalmanResult {
    std::vector<Eigen::Vector2d> filtered_state;
    std::vector<Eigen::Matrix2d> filtered_cov;
    std::vector<Eigen::Vector2d> predicted_state;
    std::vector<Eigen::Matrix2d> predicted_cov;
    std::vector<double>          innovations;
    std::vector<double>          innovation_var;
    double log_likelihood = 0.0;
};

struct SmoothedResult {
    std::vector<Eigen::Vector2d> smoothed_state;
    std::vector<Eigen::Matrix2d> smoothed_cov;
};

struct SeriesPosterior {
    std::string         name;
    bool                is_etf;
    std::vector<double> raw_values;
    KalmanResult        kf;
    SmoothedResult      sm;
};

struct PCAResult {
    Eigen::MatrixXd transformed;
    Eigen::MatrixXd components;
    Eigen::VectorXd mean;
    Eigen::VectorXd std_dev;
    Eigen::VectorXd explained_variance_ratio;
};

struct GMMResult {
    int k = 0;
    Eigen::VectorXd              weights;
    std::vector<Eigen::VectorXd> means;
    std::vector<Eigen::MatrixXd> covariances;
    Eigen::MatrixXd              responsibilities;
    Eigen::VectorXi              labels;
    Eigen::VectorXd              entropy;
};

// ─────────────────────────────────────────────────────────────────────────────
// CSV I/O
// ─────────────────────────────────────────────────────────────────────────────

static std::vector<std::string> split_csv_line(const std::string& line) {
    std::vector<std::string> tokens;
    std::string cur;
    bool in_quotes = false;
    for (char ch : line) {
        if (ch == '"') { in_quotes = !in_quotes; }
        else if (ch == ',' && !in_quotes) { tokens.push_back(cur); cur.clear(); }
        else { cur.push_back(ch); }
    }
    tokens.push_back(cur);
    return tokens;
}

static double to_double(const std::string& s) {
    if (s.empty()) return std::numeric_limits<double>::quiet_NaN();
    try { return std::stod(s); }
    catch (...) { return std::numeric_limits<double>::quiet_NaN(); }
}

static DataFrame read_csv(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("Cannot open: " + path);

    std::string line;
    if (!std::getline(in, line)) throw std::runtime_error("CSV empty.");

    auto header = split_csv_line(line);
    if (header.empty() || header[0] != "date")
        throw std::runtime_error("CSV must begin with 'date' column.");

    std::vector<std::string> columns(header.begin() + 1, header.end());
    for (auto& c : columns) {
        while (!c.empty() && (c.back() == '\r' || c.back() == ' ')) c.pop_back();
    }

    std::vector<std::string> dates;
    std::vector<std::vector<double>> rows;

    while (std::getline(in, line)) {
        if (line.empty() || line == "\r") continue;
        auto tokens = split_csv_line(line);
        if ((int)tokens.size() != (int)header.size()) continue;
        dates.push_back(tokens[0]);
        std::vector<double> row;
        for (size_t j = 1; j < tokens.size(); ++j) row.push_back(to_double(tokens[j]));
        rows.push_back(row);
    }
    if (rows.empty()) throw std::runtime_error("No data rows in CSV.");

    const int T = (int)rows.size(), N = (int)columns.size();
    Eigen::MatrixXd values(T, N);
    for (int i = 0; i < T; ++i)
        for (int j = 0; j < N; ++j)
            values(i, j) = rows[i][j];

    return {dates, columns, values};
}

// ─────────────────────────────────────────────────────────────────────────────
// DATA CLEANING
// ─────────────────────────────────────────────────────────────────────────────

static void forward_fill(Eigen::MatrixXd& X) {
    for (int j = 0; j < X.cols(); ++j) {
        double last = std::numeric_limits<double>::quiet_NaN();
        for (int i = 0; i < X.rows(); ++i) {
            if (std::isfinite(X(i,j))) last = X(i,j);
            else if (std::isfinite(last)) X(i,j) = last;
        }
    }
}

static void backward_fill(Eigen::MatrixXd& X) {
    for (int j = 0; j < X.cols(); ++j) {
        double next = std::numeric_limits<double>::quiet_NaN();
        for (int i = X.rows()-1; i >= 0; --i) {
            if (std::isfinite(X(i,j))) next = X(i,j);
            else if (std::isfinite(next)) X(i,j) = next;
        }
    }
}

static int col_idx(const std::vector<std::string>& cols, const std::string& name) {
    for (int i = 0; i < (int)cols.size(); ++i) if (cols[i] == name) return i;
    throw std::runtime_error("Column not found: '" + name + "'");
}

static std::vector<double> extract_col(const Eigen::MatrixXd& X, int j) {
    std::vector<double> v(X.rows());
    for (int i = 0; i < X.rows(); ++i) v[i] = X(i, j);
    return v;
}

// ─────────────────────────────────────────────────────────────────────────────
// BSTS — LOCAL LINEAR TREND
// ─────────────────────────────────────────────────────────────────────────────

static KalmanResult run_kalman(
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

static SmoothedResult run_rts(const KalmanResult& kf) {
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

static std::tuple<double,double,double> mle_sigma(const std::vector<double>& y) {
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

static SeriesPosterior fit_series(
    const std::string& name, const std::vector<double>& raw, bool is_etf)
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
        // Patch any NaNs via forward/backward fill
        double last = 0.0; bool found = false;
        for (auto& v : sp.raw_values) {
            if (std::isfinite(v)) { last = v; found = true; }
            else if (found) v = last;
        }
        double next = 0.0; found = false;
        for (int i = (int)sp.raw_values.size()-1; i >= 0; --i) {
            if (std::isfinite(sp.raw_values[i])) { next = sp.raw_values[i]; found = true; }
            else if (found) sp.raw_values[i] = next;
        }
    }

    auto [so, sl, ss] = mle_sigma(sp.raw_values);
    sp.kf = run_kalman(sp.raw_values, so, sl, ss);
    sp.sm = run_rts(sp.kf);
    return sp;
}

// ─────────────────────────────────────────────────────────────────────────────
// HELPERS
// ─────────────────────────────────────────────────────────────────────────────

static double smooth_level(const SeriesPosterior& sp, int t) {
    return sp.sm.smoothed_state[t](0);
}
static double smooth_slope(const SeriesPosterior& sp, int t) {
    return sp.sm.smoothed_state[t](1);
}

static Eigen::VectorXd rolling_innov_vol(const std::vector<double>& innov, int win) {
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

static Eigen::VectorXd gaussian_smooth(const Eigen::VectorXd& x,
                                        int radius = 4, double sigma = 2.0) {
    const int n = (int)x.size();
    std::vector<double> kernel;
    double ks = 0;
    for (int k = -radius; k <= radius; ++k) {
        double v = std::exp(-(double)(k*k) / (2.0*sigma*sigma));
        kernel.push_back(v); ks += v;
    }
    for (auto& v : kernel) v /= ks;

    Eigen::VectorXd out(n);
    for (int i = 0; i < n; ++i) {
        double acc = 0;
        for (int k = -radius; k <= radius; ++k)
            acc += kernel[k+radius] * x(std::clamp(i+k, 0, n-1));
        out(i) = acc;
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// FEATURE EXTRACTION  (50-dim)
// ─────────────────────────────────────────────────────────────────────────────
// Block 1 (32): ETF β_t, σ_β, ∇²μ, innov_vol
// Block 2 (12): FRED smoothed μ_t per macro series
// Block 3 ( 6): growth_score, inflation_score, growth_inflation_quad,
//               yield_curve, financial_stress, labor_slack

static Eigen::MatrixXd build_feature_matrix(
    const std::vector<SeriesPosterior>& etfs,
    const std::vector<SeriesPosterior>& macros,
    const std::unordered_map<std::string, int>& midx,
    std::vector<std::string>& feat_names,
    Eigen::VectorXd& ros_out,
    Eigen::VectorXd& growth_out,
    Eigen::VectorXd& inflation_out,
    Eigen::VectorXd& gi_quad_out)
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
        Eigen::VectorXd ivol = rolling_innov_vol(sp.kf.innovations, kInnovWin);
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

    for (int t = 0; t < T; ++t) {
        // Growth score: industrial production slope + PMI deviation from 50 + GDP slope
        const double gs =
            (mslope("indpro_yoy", t) + mlevel("cfnai",t) + mslope("gdp_qoq",t)) / 3.0;

        // Inflation score: average of CPI, PCE, breakeven (all in % units)
        const double is =
            (mlevel("cpi_yoy",t) + mlevel("pce_yoy",t) + mlevel("breakeven_10y",t)) / 3.0;

        // Growth-Inflation quadrant:
        //   positive  → reflationary (growth outpacing inflation)
        //   negative  → stagflationary / risk-off
        const double gi = gs - is * 0.5;

        // Yield curve: 10Y-2Y spread (positive=normal, negative=inverted)
        const double yc = mlevel("t10y2y", t);

        // Financial stress: HY OAS (high=stress=risk-off)
        const double fs = mlevel("hy_oas", t);

        // Labor slack: slope of unemployment (rising=deteriorating=risk-off)
        const double ls = mslope("unrate", t);

        // Risk-On Score: ETF β-based (SPY=0, EEM=1, TLT=2, GLD=4)
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

    // Gaussian smooth polarity composites (8-week)
    for (int c = b3; c < b3+6; ++c)
        X.col(c) = gaussian_smooth(X.col(c), 4, 2.0);
    ros        = gaussian_smooth(ros,        4, 2.0);
    growth_v   = gaussian_smooth(growth_v,   4, 2.0);
    inflation_v = gaussian_smooth(inflation_v, 4, 2.0);
    gi_quad    = gaussian_smooth(gi_quad,    4, 2.0);

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

// ─────────────────────────────────────────────────────────────────────────────
// PCA
// ─────────────────────────────────────────────────────────────────────────────

static PCAResult run_pca(const Eigen::MatrixXd& X, int n_comp) {
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

    out.components             = vecs;
    out.transformed            = Xs * vecs;
    out.explained_variance_ratio = vals / (vals.sum() + kEps);
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// FULL-COVARIANCE GMM
// ─────────────────────────────────────────────────────────────────────────────

static double log_mvn_pdf(
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

// ─────────────────────────────────────────────────────────────────────────────
// K-MEANS++ INITIALISATION
// Spreads initial centroids proportional to squared distance from already-chosen
// centres. Dramatically reduces the chance of degenerate GMM solutions where
// one cluster captures almost all observations.
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<Eigen::VectorXd> kmeans_plus_plus(
    const Eigen::MatrixXd& X, int K, std::mt19937& rng)
{
    const int n = (int)X.rows();
    std::vector<Eigen::VectorXd> centres;
    centres.reserve(K);

    // 1. Pick first centre uniformly at random
    std::uniform_int_distribution<int> uniform(0, n-1);
    centres.push_back(X.row(uniform(rng)).transpose());

    // 2. Each subsequent centre chosen with probability proportional to D²
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
        // Sample proportional to D²
        std::discrete_distribution<int> weighted(
            dists.data(), dists.data() + n);
        centres.push_back(X.row(weighted(rng)).transpose());
    }
    return centres;
}

// ─────────────────────────────────────────────────────────────────────────────
// ONE EM RUN — single initialisation, returns log-likelihood and full result
// ─────────────────────────────────────────────────────────────────────────────
static std::pair<double, GMMResult> run_em(
    const Eigen::MatrixXd& X, int K,
    const std::vector<Eigen::VectorXd>& init_means)
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

    for (int iter = 0; iter < kGMMIter; ++iter) {
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
        if (std::abs(ll - prev_ll) < kGMMTol) { prev_ll = ll; break; }
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

// ─────────────────────────────────────────────────────────────────────────────
// FIT GMM — K-means++ init × kGMMRestarts, keep best log-likelihood
// ─────────────────────────────────────────────────────────────────────────────
static GMMResult fit_gmm(const Eigen::MatrixXd& X, int K, int seed = 42) {
    const int n = (int)X.rows();
    if (n < K) throw std::invalid_argument("Too few observations for GMM.");

    std::mt19937 rng(seed);
    double best_ll = -std::numeric_limits<double>::infinity();
    GMMResult best;

    for (int restart = 0; restart < kGMMRestarts; ++restart) {
        auto centres = kmeans_plus_plus(X, K, rng);
        auto [ll, result] = run_em(X, K, centres);
        std::cerr << "    restart " << (restart+1) << "/" << kGMMRestarts
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

// ─────────────────────────────────────────────────────────────────────────────
// REGIME LABELLING — 5-dim polarity map
// Dimensions: {equity_β, bond_β, commodity_β, gi_quad, financial_stress}
// ─────────────────────────────────────────────────────────────────────────────

static const char* REGIME_NAMES[4] = {
    "R0 Risk-On Growth", "R1 Risk-Off/Crash", "R2 Stagflation", "R3 Reflation"
};

// label_regimes
// ─────────────────────────────────────────────────────────────────────────────
// Labels raw GMM cluster indices → semantic regime names using the macro
// composite scores directly, not raw ETF β polarity.
//
// This ensures the printed regime name, the posteriors, and the macro
// composite diagnostics are always consistent with each other — they all
// derive from the same underlying BSTS-smoothed fundamental signals.
//
// Per-cluster mean composites used for scoring:
//   growth_score          (positive = expanding)
//   inflation_score       (higher = more inflation pressure)
//   gi_quad               (positive = growth>inflation, negative = stagflationary)
//   financial_stress      (higher = more credit/vol stress)
//   ros                   (risk-on score from ETF β — kept as tiebreaker)
//
// Regime signatures in this space:
//   R0 Risk-On Growth:  gi_quad↑↑, growth↑, inflation~,  stress↓,  ros↑
//   R1 Risk-Off/Crash:  gi_quad↓,  growth↓, inflation~,  stress↑↑, ros↓
//   R2 Stagflation:     gi_quad↓↓, growth↓, inflation↑↑, stress~,  ros↓
//   R3 Reflation:       gi_quad↑,  growth↑, inflation↑,  stress↓,  ros↑
//
// Scoring matrix columns: {gi_quad, growth, inflation, stress, ros}
static const double MACRO_SIGNATURES[4][5] = {
    { 2.0,  1.0,  0.0, -1.0,  1.0},  // R0 Risk-On Growth
    {-1.0, -1.0,  0.0,  2.0, -1.0},  // R1 Risk-Off/Crash
    {-2.0, -1.0,  1.5,  0.0, -1.0},  // R2 Stagflation   (gi_quad most discriminating)
    { 1.0,  1.0,  0.5, -1.0,  1.0},  // R3 Reflation     (growth + moderate inflation)
};

static std::vector<int> label_regimes(
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
    // by virtue of its units (e.g. init_claims is in thousands, gi_quad is ~1)
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

    // Greedy one-to-one assignment: best (score, cluster, label) pairs first
    std::vector<int> mapping(K, -1);
    std::vector<bool> used_raw(K, false), used_lbl(K, false);
    for (auto& [score, raw, lbl] : cells) {
        if (!used_raw[raw] && !used_lbl[lbl]) {
            mapping[raw] = lbl;
            used_raw[raw] = used_lbl[lbl] = true;
            std::cerr << "  assign cluster " << raw
                      << " -> " << REGIME_NAMES[lbl]
                      << "  (score=" << std::setprecision(3) << score << ")\n";
        }
    }
    // Fill any unmatched (shouldn't happen with K=4 balanced clusters)
    int nxt = 0;
    for (int k = 0; k < K; ++k) {
        if (mapping[k] == -1) {
            while (nxt < K && used_lbl[nxt]) ++nxt;
            mapping[k] = (nxt < K) ? nxt++ : k;
        }
    }
    return mapping;
}

// ─────────────────────────────────────────────────────────────────────────────
// OUTPUT
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// SPOT REGIME — derive current-week label directly from macro composites
// This is independent of the GMM cluster assignment and always consistent
// with the printed macro composite values.
//
// Decision logic mirrors the MACRO_SIGNATURES matrix but applied point-wise:
//   Primary discriminator:  GI-quad (growth vs inflation balance)
//   Secondary:              financial stress (risk-off severity)
//   Tertiary:               growth score direction
// ─────────────────────────────────────────────────────────────────────────────
static int spot_regime(double gi, double gs, double inflation, double stress) {
    // Stagflation: gi strongly negative (growth well below inflation)
    if (gi < -0.5 && gs <= 0)   return 2;  // R2 Stagflation
    // Risk-Off/Crash: high stress regardless of other signals
    if (stress > 1.0 && gs <= 0) return 1;  // R1 Risk-Off/Crash
    // Risk-Off: negative gi + contracting growth even without extreme stress
    if (gi < -0.2 && gs <= 0)   return 1;  // R1 Risk-Off/Crash
    // Reflation: positive gi + expanding growth + moderate-to-high inflation
    if (gi > 0 && gs > 0 && inflation > 2.0) return 3;  // R3 Reflation
    // Risk-On Growth: positive gi + expanding, low inflation pressure
    if (gi > 0 && gs > 0)       return 0;  // R0 Risk-On Growth
    // Disinflation edge case: gi positive but growth contracting
    // (slowdown with falling inflation — closest to risk-off)
    if (gi > 0 && gs <= 0)      return 1;  // R1 Risk-Off/Crash
    // Default fallback
    return 1;
}

static void print_summary(
    const GMMResult& gmm,
    const std::vector<int>& lmap,
    const std::vector<SeriesPosterior>& etfs,
    const std::vector<SeriesPosterior>& macros,
    const PCAResult& pca,
    const Eigen::VectorXd& ros,
    const Eigen::VectorXd& growth,
    const Eigen::VectorXd& inflation,
    const Eigen::VectorXd& gi_quad,
    const Eigen::VectorXd& fin_stress,
    int T)
{
    const int t   = T - 1;

    // GMM-based label (structural cluster membership)
    const int raw     = gmm.labels(t);
    const int gmm_reg = lmap[raw];

    // Spot label (current-week macro composites — always consistent with diagnostics)
    const int spot_reg = spot_regime(gi_quad(t), growth(t), inflation(t), fin_stress(t));

    // Use spot label for the header; show GMM label separately for reference
    const int display_reg = spot_reg;

    std::vector<double> post(4, 0.0);
    for (int k = 0; k < gmm.k; ++k) post[lmap[k]] = gmm.responsibilities(t, k);

    double cum = 0;
    for (int k = 0; k < pca.transformed.cols(); ++k) cum += pca.explained_variance_ratio(k);

    std::cout << std::fixed;
    std::cout << "\n╔══════════════════════════════════════════════════════════════════╗\n";
    std::cout <<   "║  BSTS MULTI-ASSET MACRO REGIME DETECTOR                         ║\n";
    std::cout <<   "║  AlgoGators Investment Fund  ·  Quantitative Research            ║\n";
    std::cout <<   "╚══════════════════════════════════════════════════════════════════╝\n";
    std::cout << "  Observations: " << T << " weekly\n";
    std::cout << "  ETF series:   " << etfs.size()   << "  (market momentum — Block 1)\n";
    std::cout << "  Macro series: " << macros.size() << "  (fundamental drivers — Block 2)\n";
    std::cout << "  PCA:          " << pca.transformed.cols() << " components  ("
              << std::setprecision(1) << cum*100 << "% var explained)\n";
    std::cout << "  GMM:          " << gmm.k << " regimes\n";

    std::cout << "\n  ┌──────────────────────────────────────────────────────────────┐\n";
    std::cout <<   "  │  CURRENT REGIME                                              │\n";
    std::cout << "  │  " << std::left << std::setw(61) << REGIME_NAMES[display_reg] << "│\n";
    std::cout << "  │  GMM structural label: "
              << std::setw(40) << REGIME_NAMES[gmm_reg] << "│\n";
    std::cout << "  │  Entropy  " << std::setprecision(4) << gmm.entropy(t)
              << "   " << std::setw(43)
              << (gmm.entropy(t) <= kHighConviction
                  ? "HIGH CONVICTION (entropy <= 0.20)"
                  : "LOW — near transition zone") << "│\n";
    std::cout <<   "  └──────────────────────────────────────────────────────────────┘\n";

    std::cout << "\n  POSTERIOR PROBABILITIES  P(regime_k | BSTS features)\n";
    std::cout <<   "  ──────────────────────────────────────────────────────────────\n";
    for (int l = 0; l < 4; ++l) {
        int bar = (int)(post[l] * 40);
        std::cout << "  " << std::left << std::setw(22) << REGIME_NAMES[l]
                  << "  " << std::right << std::setprecision(4) << std::setw(6) << post[l]
                  << "  " << std::string(bar, '#') << (l == display_reg ? "  <" : "") << "\n";
    }

    std::cout << "\n  MACRO COMPOSITES  (current week)\n";
    std::cout <<   "  ──────────────────────────────────────────────────────────────\n";
    std::cout << "  Risk-On Score (ETF β)       = " << std::showpos << std::setprecision(5)
              << ros(t) << std::noshowpos
              << "  →  " << (ros(t) > 0 ? "RISK-ON" : "RISK-OFF") << "\n";
    std::cout << "  Growth Score  (PMI/IP/GDP)  = " << std::showpos << std::setprecision(3)
              << growth(t) << std::noshowpos
              << "  →  " << (growth(t) > 0 ? "EXPANDING" : "CONTRACTING") << "\n";
    std::cout << "  Inflation Score (CPI/PCE/BE) = " << std::setprecision(3)
              << inflation(t) << "%\n";
    std::cout << "  Growth-Inflation Quad        = " << std::showpos << std::setprecision(3)
              << gi_quad(t) << std::noshowpos << "\n";

    const char* quad =
        (growth(t) > 0 && gi_quad(t) > 0)  ? "REFLATIONARY (Growth > Inflation)" :
        (growth(t) > 0 && gi_quad(t) <= 0) ? "OVERHEATING  (Growth + High Inflation)" :
        (growth(t) <= 0 && gi_quad(t) > 0) ? "DISINFLATION (Slowdown, Easing Prices)" :
                                              "STAGFLATION  (Low Growth + High Inflation)";
    std::cout << "                               →  " << quad << "\n";

    std::cout << "\n  PER-ASSET SMOOTHED SLOPE beta_T\n";
    std::cout <<   "  ──────────────────────────────────────────────────────────────\n";
    for (const auto& sp : etfs) {
        double b  = smooth_slope(sp, t);
        double sb = std::sqrt(std::max(0.0, sp.sm.smoothed_cov[t](1,1)));
        int bar   = std::min(20, (int)(std::abs(b)/0.001));
        std::cout << "  " << std::left << std::setw(5) << sp.name
                  << "  beta=" << std::showpos << std::setprecision(5) << b << std::noshowpos
                  << "  sigma_b=" << std::setprecision(5) << sb
                  << "  " << (b >= 0 ? std::string(bar,'^') : std::string(bar,'v')) << "\n";
    }

    std::cout << "\n  KEY MACRO LEVELS  (BSTS smoothed μ_t)\n";
    std::cout <<   "  ──────────────────────────────────────────────────────────────\n";
    for (const auto& sp : macros) {
        double lv = smooth_level(sp, t);
        double sl = smooth_slope(sp, t);
        std::cout << "  " << std::left << std::setw(20) << sp.name
                  << "  μ=" << std::right << std::setprecision(3) << std::setw(8) << lv
                  << "  slope=" << std::showpos << std::setprecision(4) << sl
                  << std::noshowpos << "\n";
    }

    std::cout << "\n  REGIME DISTRIBUTION  (full sample)\n";
    std::cout <<   "  ──────────────────────────────────────────────────────────────\n";
    for (int l = 0; l < 4; ++l) {
        int cnt = 0;
        for (int tt = 0; tt < T; ++tt) if (lmap[gmm.labels(tt)] == l) ++cnt;
        double pct = 100.0*cnt/T;
        std::cout << "  " << std::left << std::setw(22) << REGIME_NAMES[l]
                  << "  " << std::setprecision(1) << std::right << std::setw(5) << pct << "%"
                  << "  " << std::string((int)(pct/3), '#') << "\n";
    }

    std::cout << "\n  REGIME PLAYBOOK\n";
    std::cout <<   "  ──────────────────────────────────────────────────────────────\n";
    const char* longs[4]  = {"SP500,EM,Copper",  "Bonds,Gold,USD","Gold,Oil","Equities,Oil,Gold"};
    const char* shorts[4] = {"Bonds,USD","Equities,Oil,HY","Bonds,EM","Bonds,USD"};
    const char* watch[4]  = {"Oil momentum","Vol carry","Real assets","HY spread tightening"};
    std::cout << "  " << std::left << std::setw(22) << "Regime"
              << std::setw(20) << "Long" << std::setw(18) << "Short" << "Watch\n";
    std::cout << "  " << std::string(78,'-') << "\n";
    for (int l = 0; l < 4; ++l)
        std::cout << "  " << std::setw(22) << REGIME_NAMES[l]
                  << std::setw(20) << longs[l] << std::setw(18) << shorts[l]
                  << watch[l] << (l == display_reg ? "  <- NOW" : "") << "\n";
    std::cout << "\n";
}

static void write_output_csv(
    const std::string& path,
    const std::vector<std::string>& dates,
    const std::vector<SeriesPosterior>& etfs,
    const std::vector<SeriesPosterior>& macros,
    const Eigen::MatrixXd& features,
    const std::vector<std::string>& feat_names,
    const PCAResult& pca,
    const GMMResult& gmm,
    const std::vector<int>& lmap,
    const Eigen::VectorXd& ros,
    const Eigen::VectorXd& growth,
    const Eigen::VectorXd& inflation,
    const Eigen::VectorXd& gi_quad,
    const Eigen::VectorXd& fin_stress)
{
    std::ofstream out(path);
    if (!out) throw std::runtime_error("Cannot write: " + path);

    out << "date";
    for (const auto& sp : etfs)
        out << "," << sp.name << "_mu" << "," << sp.name << "_beta"
            << "," << sp.name << "_sigma_beta";
    for (const auto& sp : macros)
        out << "," << sp.name << "_mu" << "," << sp.name << "_slope";
    for (const auto& f : feat_names) out << "," << f;
    for (int j = 0; j < pca.transformed.cols(); ++j) out << ",pc" << (j+1);
    out << ",risk_on_score,growth_score,inflation_score,growth_inflation_quad";
    for (int j = 0; j < gmm.k; ++j) out << ",prob_" << REGIME_NAMES[j];
    out << ",regime_raw,regime_gmm_label,regime_gmm_name";
    out << ",regime_spot_label,regime_spot_name,entropy\n";

    const int T = (int)dates.size();
    for (int t = 0; t < T; ++t) {
        out << dates[t];
        for (const auto& sp : etfs)
            out << "," << smooth_level(sp,t) << "," << smooth_slope(sp,t)
                << "," << std::sqrt(std::max(0.0, sp.sm.smoothed_cov[t](1,1)));
        for (const auto& sp : macros)
            out << "," << smooth_level(sp,t) << "," << smooth_slope(sp,t);
        for (int j = 0; j < features.cols(); ++j) out << "," << features(t,j);
        for (int j = 0; j < pca.transformed.cols(); ++j) out << "," << pca.transformed(t,j);
        out << "," << ros(t) << "," << growth(t)
            << "," << inflation(t) << "," << gi_quad(t);

        std::vector<double> post(gmm.k, 0.0);
        for (int k = 0; k < gmm.k; ++k) post[lmap[k]] = gmm.responsibilities(t,k);
        for (int j = 0; j < gmm.k; ++j) out << "," << post[j];

        const int raw      = gmm.labels(t);
        const int gmm_reg  = lmap[raw];
        const int spot_reg = spot_regime(gi_quad(t), growth(t), inflation(t), fin_stress(t));
        out << "," << raw
            << "," << gmm_reg  << "," << REGIME_NAMES[gmm_reg]
            << "," << spot_reg << "," << REGIME_NAMES[spot_reg]
            << "," << gmm.entropy(t) << "\n";
    }
    std::cerr << "[output] regime CSV -> " << path << "\n";
}

static void write_summary_csv(
    const std::string& path,
    const GMMResult& gmm,
    const std::vector<int>& lmap,
    const Eigen::MatrixXd& features,
    const std::vector<std::string>& feat_names)
{
    std::ofstream out(path);
    if (!out) throw std::runtime_error("Cannot write: " + path);
    out << "regime_raw,regime_label,regime_name,weight";
    for (const auto& f : feat_names) out << "," << f << "_mean";
    out << "\n";
    const int T = (int)gmm.labels.size();
    for (int k = 0; k < gmm.k; ++k) {
        Eigen::RowVectorXd avg = Eigen::RowVectorXd::Zero(features.cols());
        double cnt = 0;
        for (int t = 0; t < T; ++t)
            if (gmm.labels(t) == k) { avg += features.row(t); ++cnt; }
        if (cnt > 0) avg /= cnt;
        out << k << "," << lmap[k] << "," << REGIME_NAMES[lmap[k]] << "," << gmm.weights(k);
        for (int j = 0; j < avg.size(); ++j) out << "," << avg(j);
        out << "\n";
    }
    std::cerr << "[output] summary CSV -> " << path << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// MAIN
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    try {
        if (argc < 3) {
            std::cerr << "Usage: " << argv[0]
                      << " input.csv output_regimes.csv [summary.csv]\n";
            return 1;
        }
        const std::string in_path  = argv[1];
        const std::string out_path = argv[2];
        const std::string sum_path = (argc >= 4) ? argv[3] : "regime_summary.csv";

        // 1. Load
        std::cerr << "[data] reading " << in_path << "\n";
        DataFrame df = read_csv(in_path);
        forward_fill(df.values);
        backward_fill(df.values);
        for (const auto& s : ETF_SERIES)   col_idx(df.columns, s);
        for (const auto& s : MACRO_SERIES) col_idx(df.columns, s);
        const int T = (int)df.values.rows();
        std::cerr << "[data] " << T << " weekly rows\n";

        // 2. BSTS — ETF series (log-price)
        std::cerr << "[bsts] ETF series...\n";
        std::vector<SeriesPosterior> etf_posts;
        for (const auto& name : ETF_SERIES) {
            std::cerr << "  " << std::left << std::setw(6) << name;
            auto sp = fit_series(name, extract_col(df.values, col_idx(df.columns, name)), true);
            std::cerr << "  LL=" << std::setprecision(1) << sp.kf.log_likelihood << "\n";
            etf_posts.push_back(std::move(sp));
        }

        // 3. BSTS — macro series (raw levels/rates)
        std::cerr << "[bsts] macro series...\n";
        std::vector<SeriesPosterior> mac_posts;
        std::unordered_map<std::string, int> midx;
        for (int m = 0; m < (int)MACRO_SERIES.size(); ++m) {
            const auto& name = MACRO_SERIES[m];
            std::cerr << "  " << std::left << std::setw(22) << name;
            auto sp = fit_series(name, extract_col(df.values, col_idx(df.columns, name)), false);
            std::cerr << "  LL=" << std::setprecision(1) << sp.kf.log_likelihood << "\n";
            midx[name] = m;
            mac_posts.push_back(std::move(sp));
        }

        // 4. Feature matrix (50-dim)
        std::cerr << "[features] building " << T << " x 50 matrix\n";
        Eigen::VectorXd ros, growth, inflation, gi_quad;
        std::vector<std::string> feat_names;
        Eigen::MatrixXd features = build_feature_matrix(
            etf_posts, mac_posts, midx,
            feat_names, ros, growth, inflation, gi_quad);

        // 5. PCA
        const int npca = std::min(kPCAComponents, (int)features.cols());
        std::cerr << "[pca] " << npca << " components\n";
        PCAResult pca = run_pca(features, npca);
        double cum = 0;
        for (int k = 0; k < npca; ++k) cum += pca.explained_variance_ratio(k);
        std::cerr << "  variance explained: " << std::setprecision(1) << cum*100 << "%\n";

        // 6. GMM
        std::cerr << "[gmm] K=" << kGMMRegimes
                  << "  restarts=" << kGMMRestarts
                  << "  (K-means++ init)\n";
        GMMResult gmm = fit_gmm(pca.transformed, kGMMRegimes);
        double pct_hc = 100.0 *
            (gmm.entropy.array() <= kHighConviction).cast<double>().mean();
        std::cerr << "  high-conviction obs: " << std::setprecision(1) << pct_hc << "%\n";

        // 7. Label regimes using macro composites directly
        // fin_stress from smoothed hy_oas level in Block 2
        const int hy_col = (int)ETF_SERIES.size()*4 + midx.at("hy_oas");
        Eigen::VectorXd fin_stress = features.col(hy_col);
        std::cerr << "[labelling] cluster macro composite profiles:\n";
        std::vector<int> lmap = label_regimes(
            gmm, growth, inflation, gi_quad, fin_stress, ros);

        // 8. Output
        print_summary(gmm, lmap, etf_posts, mac_posts, pca,
                      ros, growth, inflation, gi_quad, fin_stress, T);
        write_output_csv(out_path, df.dates, etf_posts, mac_posts,
                         features, feat_names, pca, gmm, lmap,
                         ros, growth, inflation, gi_quad, fin_stress);
        write_summary_csv(sum_path, gmm, lmap, features, feat_names);

        std::cout << "Done. Rows=" << T << "  Regimes=" << kGMMRegimes << "\n";
        std::cout << "Output:  " << out_path  << "\n";
        std::cout << "Summary: " << sum_path  << "\n";

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}