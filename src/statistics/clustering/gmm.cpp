#include "trade_ngin/statistics/clustering/gmm.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace trade_ngin {
namespace statistics {

static constexpr double kPi  = 3.14159265358979323846;
static constexpr double kEps = 1e-8;

// ============================================================================
// Constructor
// ============================================================================

GMM::Config::Config() = default;

GMM::GMM(Config config) : config_(std::move(config)) {}

// ============================================================================
// log N(x | mu, Sigma) — multivariate normal log-density
// ============================================================================

double GMM::log_mvn_pdf(
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
    return -0.5 * (d * std::log(2.0 * kPi) + log_det + llt.solve(diff).dot(diff));
}

// ============================================================================
// K-means++ initialisation
// ============================================================================

std::vector<Eigen::VectorXd> GMM::kmeans_plus_plus(
    const Eigen::MatrixXd& X, int K, std::mt19937& rng)
{
    const int n = (int)X.rows();
    std::vector<Eigen::VectorXd> centres;
    centres.reserve(K);

    // First centre: uniform random
    std::uniform_int_distribution<int> uniform(0, n - 1);
    centres.push_back(X.row(uniform(rng)).transpose());

    // Subsequent centres: probability proportional to D^2
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
        std::discrete_distribution<int> weighted(
            dists.data(), dists.data() + n);
        centres.push_back(X.row(weighted(rng)).transpose());
    }
    return centres;
}

// ============================================================================
// Single EM run — returns (log-likelihood, GMMResult)
// ============================================================================

std::pair<double, GMMResult> GMM::run_em(
    const Eigen::MatrixXd& X, int K,
    const std::vector<Eigen::VectorXd>& init_means) const
{
    const int n = (int)X.rows(), d = (int)X.cols();

    GMMResult out;
    out.k = K;
    out.weights = Eigen::VectorXd::Constant(K, 1.0 / K);
    out.means = init_means;
    out.covariances.resize(K);
    out.responsibilities = Eigen::MatrixXd::Zero(n, K);

    for (int j = 0; j < K; ++j)
        out.covariances[j] = Eigen::MatrixXd::Identity(d, d);

    double prev_ll = -std::numeric_limits<double>::infinity();

    for (int iter = 0; iter < config_.max_iterations; ++iter) {
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
        if (std::abs(ll - prev_ll) < config_.tolerance) { prev_ll = ll; break; }
        prev_ll = ll;

        // M-step
        for (int j = 0; j < K; ++j) {
            double Nj = out.responsibilities.col(j).sum();
            out.weights(j) = Nj / n;

            Eigen::VectorXd mu = Eigen::VectorXd::Zero(d);
            for (int i = 0; i < n; ++i)
                mu += out.responsibilities(i, j) * X.row(i).transpose();
            mu /= std::max(kEps, Nj);
            out.means[j] = mu;

            Eigen::MatrixXd cov = Eigen::MatrixXd::Zero(d, d);
            for (int i = 0; i < n; ++i) {
                Eigen::VectorXd diff = X.row(i).transpose() - mu;
                cov += out.responsibilities(i, j) * diff * diff.transpose();
            }
            cov /= std::max(kEps, Nj);
            cov += 1e-4 * Eigen::MatrixXd::Identity(d, d);
            out.covariances[j] = cov;
        }
    }
    return {prev_ll, std::move(out)};
}

// ============================================================================
// fit — multi-restart K-means++ + EM, keep best log-likelihood
// ============================================================================

GMMResult GMM::fit(const Eigen::MatrixXd& X, int K, int seed) const
{
    const int n = (int)X.rows();
    if (n < K)
        throw std::invalid_argument("Too few observations for GMM.");

    std::mt19937 rng(seed);
    double best_ll = -std::numeric_limits<double>::infinity();
    GMMResult best;

    for (int restart = 0; restart < config_.restarts; ++restart) {
        auto centres = kmeans_plus_plus(X, K, rng);
        auto [ll, result] = run_em(X, K, centres);
        std::cerr << "    GMM restart " << (restart + 1) << "/" << config_.restarts
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
            double p = best.responsibilities(i, j);
            if (p > 1e-10) h -= p * std::log(p);
        }
        // L-10: guard against K=1 → log(1)=0 → divide by zero NaN.
        // For a degenerate single-cluster fit, entropy is by definition 0
        // (no uncertainty about which cluster the point belongs to).
        best.entropy(i) = (K > 1) ? h / std::log((double)K) : 0.0;
    }

    std::cerr << "  GMM best ll=" << std::fixed << std::setprecision(1) << best_ll << "\n";
    return best;
}

// ============================================================================
// predict_proba — evaluate cluster probabilities for a single new point
// ============================================================================

Eigen::VectorXd GMM::predict_proba(
    const Eigen::VectorXd& x, const GMMResult& model)
{
    const int K = model.k;
    Eigen::VectorXd logp(K);

    for (int j = 0; j < K; ++j)
        logp(j) = std::log(std::max(kEps, model.weights(j)))
                 + log_mvn_pdf(x, model.means[j], model.covariances[j]);

    // Log-sum-exp normalisation
    const double lmax = logp.maxCoeff();
    Eigen::VectorXd probs = (logp.array() - lmax).exp();
    probs /= std::max(kEps, probs.sum());
    return probs;
}

} // namespace statistics
} // namespace trade_ngin
