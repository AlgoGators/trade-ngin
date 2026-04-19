#pragma once

#include <Eigen/Dense>
#include <random>
#include <utility>
#include <vector>

namespace trade_ngin {
namespace statistics {

// ============================================================================
// GMMResult — output of Gaussian Mixture Model fitting
// ============================================================================

struct GMMResult {
    int k = 0;                                    // number of clusters
    Eigen::VectorXd              weights;         // K-dim cluster priors
    std::vector<Eigen::VectorXd> means;           // K Gaussian means (d-dim)
    std::vector<Eigen::MatrixXd> covariances;     // K covariance matrices (d×d)
    Eigen::MatrixXd              responsibilities; // T×K soft assignments
    Eigen::VectorXi              labels;           // T-dim hard assignments
    Eigen::VectorXd              entropy;          // T-dim normalized Shannon entropy
};

// ============================================================================
// GMM — Gaussian Mixture Model with K-means++ init and EM
// ============================================================================

class GMM {
public:
    struct Config {
        int    max_iterations = 300;
        double tolerance      = 1e-5;
        int    restarts       = 10;
        // Declared out-of-line so DMIs above are complete before any
        // default-argument use of `Config{}` inside this enclosing class.
        Config();
    };

    explicit GMM(Config config = {});

    // Fit GMM to data matrix X (T rows × d cols) with K clusters.
    // Multi-restart: runs EM `restarts` times with K-means++ init, keeps best.
    GMMResult fit(const Eigen::MatrixXd& X, int K, int seed = 42) const;

    // Evaluate cluster probabilities for a single new observation x
    // given a fitted model. Returns K-dim probability vector.
    static Eigen::VectorXd predict_proba(
        const Eigen::VectorXd& x, const GMMResult& model);

    // Log-density of multivariate normal N(x | mu, Sigma)
    static double log_mvn_pdf(
        const Eigen::VectorXd& x,
        const Eigen::VectorXd& mu,
        const Eigen::MatrixXd& Sigma);

private:
    Config config_;

    static std::vector<Eigen::VectorXd> kmeans_plus_plus(
        const Eigen::MatrixXd& X, int K, std::mt19937& rng);

    std::pair<double, GMMResult> run_em(
        const Eigen::MatrixXd& X, int K,
        const std::vector<Eigen::VectorXd>& init_means) const;
};

} // namespace statistics
} // namespace trade_ngin
