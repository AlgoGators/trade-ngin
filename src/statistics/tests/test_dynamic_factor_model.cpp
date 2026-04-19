#include "trade_ngin/statistics/state_estimation/dynamic_factor_model.hpp"
#include <iostream>
#include <random>
#include <cmath>

using namespace trade_ngin::statistics;

// ============================================================================
// Helpers
// ============================================================================

static void pass(const std::string& name) {
    std::cout << "  [PASS] " << name << "\n";
}
static void fail(const std::string& name, const std::string& reason) {
    std::cout << "  [FAIL] " << name << " -- " << reason << "\n";
}

// Generate synthetic macro data:
//   f_t = 0.9 * f_{t-1} + noise   (K factors)
//   y_t = Lambda * f_t + e_t
static Eigen::MatrixXd make_data(int T, int N, int K, unsigned seed = 42) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> nd(0.0, 1.0);

    Eigen::MatrixXd Lambda(N, K);
    for (int n = 0; n < N; ++n)
        for (int k = 0; k < K; ++k)
            Lambda(n, k) = nd(rng);

    Eigen::MatrixXd F(T, K);
    F.row(0).setZero();
    for (int t = 1; t < T; ++t)
        for (int k = 0; k < K; ++k)
            F(t, k) = 0.9 * F(t-1, k) + 0.3 * nd(rng);

    Eigen::MatrixXd Y(T, N);
    for (int t = 0; t < T; ++t) {
        Eigen::VectorXd y = Lambda * F.row(t).transpose();
        for (int n = 0; n < N; ++n) y(n) += 0.2 * nd(rng);
        Y.row(t) = y.transpose();
    }
    return Y;
}

// ============================================================================
// Tests
// ============================================================================

bool test_fit_runs() {
    DFMConfig cfg;
    cfg.num_factors = 3;
    cfg.max_em_iterations = 50;

    DynamicFactorModel dfm(cfg);
    auto result = dfm.fit(make_data(120, 8, 3),
        {"cpi","pmi","gdp","jobs","spread","curve","m2","usd"});

    if (result.is_error()) { fail("fit_runs", result.error()->what()); return false; }
    pass("fit_runs");
    return true;
}

bool test_output_dimensions() {
    DFMConfig cfg;
    cfg.num_factors = 3;
    cfg.max_em_iterations = 50;

    DynamicFactorModel dfm(cfg);
    auto result = dfm.fit(make_data(100, 6, 3));
    if (result.is_error()) { fail("output_dimensions", result.error()->what()); return false; }

    auto& out = result.value();
    if (out.T != 100 || out.N != 6 || out.K != 3) {
        fail("output_dimensions", "T/N/K mismatch"); return false;
    }
    if ((int)out.factors.size() != 100 || (int)out.factors[0].size() != 3) {
        fail("output_dimensions", "factors shape wrong"); return false;
    }
    if (out.lambda.rows() != 6 || out.lambda.cols() != 3) {
        fail("output_dimensions", "lambda shape wrong"); return false;
    }

    pass("output_dimensions");
    return true;
}

bool test_named_factor_series() {
    DFMConfig cfg;
    cfg.num_factors   = 3;
    cfg.factor_labels = {"growth", "inflation", "liquidity"};
    cfg.max_em_iterations = 50;

    DynamicFactorModel dfm(cfg);
    auto result = dfm.fit(make_data(100, 6, 3));
    if (result.is_error()) { fail("named_factor_series", result.error()->what()); return false; }

    auto& out = result.value();
    for (auto& lbl : {"growth", "inflation", "liquidity"}) {
        if (out.factor_series.find(lbl) == out.factor_series.end()) {
            fail("named_factor_series", std::string("missing: ") + lbl); return false;
        }
        if ((int)out.factor_series.at(lbl).size() != 100) {
            fail("named_factor_series", std::string("wrong length: ") + lbl); return false;
        }
    }

    pass("named_factor_series");
    return true;
}

bool test_factors_finite() {
    DFMConfig cfg;
    cfg.num_factors       = 3;
    cfg.max_em_iterations = 100;

    DynamicFactorModel dfm(cfg);
    auto result = dfm.fit(make_data(150, 8, 3));
    if (result.is_error()) { fail("factors_finite", result.error()->what()); return false; }

    auto& out = result.value();
    for (int t = 0; t < out.T; ++t) {
        for (int k = 0; k < out.K; ++k) {
            if (!std::isfinite(out.factors[t][k])) {
                fail("factors_finite", "NaN/Inf at t=" + std::to_string(t)); return false;
            }
            if (!std::isfinite(out.factor_uncertainty[t][k])) {
                fail("factors_finite", "NaN/Inf uncertainty at t=" + std::to_string(t)); return false;
            }
        }
    }

    pass("factors_finite");
    return true;
}

bool test_convergence_info_populated() {
    DFMConfig cfg;
    cfg.num_factors       = 2;
    cfg.max_em_iterations = 100;

    DynamicFactorModel dfm(cfg);
    auto result = dfm.fit(make_data(100, 5, 2));
    if (result.is_error()) { fail("convergence_info", result.error()->what()); return false; }

    auto& out = result.value();
    if (out.convergence_info.objective_history.empty()) {
        fail("convergence_info", "objective_history is empty"); return false;
    }
    if (out.convergence_info.termination_reason.empty()) {
        fail("convergence_info", "termination_reason not set"); return false;
    }

    pass("convergence_info_populated");
    return true;
}

bool test_filter_shape() {
    DFMConfig cfg;
    cfg.num_factors       = 2;
    cfg.max_em_iterations = 50;

    DynamicFactorModel dfm(cfg);
    dfm.fit(make_data(100, 5, 2, 1));

    auto result = dfm.filter(make_data(30, 5, 2, 2));
    if (result.is_error()) { fail("filter_shape", result.error()->what()); return false; }

    auto& out = result.value();
    if (out.T != 30 || out.K != 2 || out.N != 5) {
        fail("filter_shape", "wrong dimensions"); return false;
    }

    pass("filter_shape");
    return true;
}

bool test_online_update() {
    DFMConfig cfg;
    cfg.num_factors       = 2;
    cfg.max_em_iterations = 50;

    DynamicFactorModel dfm(cfg);
    auto fit_r = dfm.fit(make_data(100, 5, 2));
    if (fit_r.is_error()) { fail("online_update", fit_r.error()->what()); return false; }

    std::mt19937 rng(99);
    std::normal_distribution<double> nd(0.0, 1.0);

    for (int i = 0; i < 10; ++i) {
        Eigen::VectorXd y(5);
        for (int n = 0; n < 5; ++n) y(n) = nd(rng);

        auto upd = dfm.update(y);
        if (upd.is_error()) { fail("online_update", upd.error()->what()); return false; }
        if (upd.value().size() != 2) { fail("online_update", "wrong size"); return false; }
        if (!upd.value().allFinite()) { fail("online_update", "non-finite"); return false; }
    }

    pass("online_update");
    return true;
}

bool test_filter_before_fit_errors() {
    DynamicFactorModel dfm;
    auto result = dfm.filter(make_data(50, 5, 3));
    if (!result.is_error()) { fail("filter_before_fit_errors", "expected error"); return false; }
    pass("filter_before_fit_errors");
    return true;
}

bool test_too_few_rows_errors() {
    DFMConfig cfg; cfg.num_factors = 3;
    DynamicFactorModel dfm(cfg);
    auto result = dfm.fit(make_data(5, 6, 3));   // only 5 rows
    if (!result.is_error()) { fail("too_few_rows_errors", "expected error"); return false; }
    pass("too_few_rows_errors");
    return true;
}

bool test_more_factors_than_series_errors() {
    DFMConfig cfg; cfg.num_factors = 5;
    DynamicFactorModel dfm(cfg);
    auto result = dfm.fit(make_data(100, 3, 3));   // 3 series < 5 factors
    if (!result.is_error()) { fail("more_factors_than_series_errors", "expected error"); return false; }
    pass("more_factors_than_series_errors");
    return true;
}

bool test_nan_handling() {
    DFMConfig cfg;
    cfg.num_factors       = 2;
    cfg.max_em_iterations = 50;

    Eigen::MatrixXd Y = make_data(100, 5, 2);
    // Punch NaN holes — simulates missing macro releases
    Y(10, 2) = std::numeric_limits<double>::quiet_NaN();
    Y(11, 2) = std::numeric_limits<double>::quiet_NaN();
    Y(50, 0) = std::numeric_limits<double>::quiet_NaN();

    DynamicFactorModel dfm(cfg);
    auto result = dfm.fit(Y);
    if (result.is_error()) { fail("nan_handling", result.error()->what()); return false; }

    auto& out = result.value();
    for (int t : {10, 11, 50}) {
        for (int k = 0; k < out.K; ++k) {
            if (!std::isfinite(out.factors[t][k])) {
                fail("nan_handling", "non-finite at NaN row t=" + std::to_string(t));
                return false;
            }
        }
    }

    pass("nan_handling");
    return true;
}

bool test_config_round_trip() {
    DFMConfig cfg;
    cfg.num_factors      = 4;
    cfg.em_tol           = 1e-5;
    cfg.standardise_data = false;
    cfg.factor_labels    = {"a","b","c","d"};

    DFMConfig cfg2;
    cfg2.from_json(cfg.to_json());

    if (cfg2.num_factors != 4 || cfg2.em_tol != 1e-5 ||
        cfg2.standardise_data != false ||
        cfg2.factor_labels != std::vector<std::string>{"a","b","c","d"}) {
        fail("config_round_trip", "round-trip mismatch"); return false;
    }

    pass("config_round_trip");
    return true;
}

bool test_ll_increases_with_more_iters() {
    Eigen::MatrixXd Y = make_data(120, 6, 2, 7);

    DFMConfig c5;  c5.num_factors = 2; c5.max_em_iterations = 5;
    DFMConfig c100; c100.num_factors = 2; c100.max_em_iterations = 100;

    DynamicFactorModel dfm5(c5), dfm100(c100);
    auto r5   = dfm5.fit(Y);
    auto r100 = dfm100.fit(Y);

    if (r5.is_error() || r100.is_error()) { fail("ll_increases", "fit failed"); return false; }
    if (r100.value().log_likelihood < r5.value().log_likelihood - 1.0) {
        fail("ll_increases",
            "100 iters LL=" + std::to_string(r100.value().log_likelihood) +
            " < 5 iters LL=" + std::to_string(r5.value().log_likelihood));
        return false;
    }

    pass("ll_increases_with_more_iters");
    return true;
}

// ============================================================================
// main
// ============================================================================

int main() {
    std::cout << "\n=== DFM Isolated Tests ===\n\n";

    int passed = 0, failed = 0;
    auto run = [&](bool(*fn)()) {
        if (fn()) ++passed; else ++failed;
    };

    run(test_fit_runs);
    run(test_output_dimensions);
    run(test_named_factor_series);
    run(test_factors_finite);
    run(test_convergence_info_populated);
    run(test_filter_shape);
    run(test_online_update);
    run(test_filter_before_fit_errors);
    run(test_too_few_rows_errors);
    run(test_more_factors_than_series_errors);
    run(test_nan_handling);
    run(test_config_round_trip);
    run(test_ll_increases_with_more_iters);

    std::cout << "\n=== " << passed << " passed, " << failed << " failed ===\n\n";
    return failed == 0 ? 0 : 1;
}
