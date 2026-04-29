// macro_msdfm_runner.cpp
// Loads macro panel from PostgreSQL → fits DFM → fits MS-DFM
// → prints regime probabilities, transition matrix, signatures
//
// Usage:
//   ./macro_msdfm_runner [connection_string] [output.csv] [start_date] [end_date]

#include "trade_ngin/regime_detection/macro/ms_dfm.hpp"
#include "trade_ngin/regime_detection/macro/macro_data_loader.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/core/logger.hpp"

#include <Eigen/Dense>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <cstdlib>

using namespace trade_ngin;
using namespace trade_ngin::statistics;

// ============================================================================
// Print helpers
// ============================================================================

static std::string regime_label(const std::vector<std::string>& labels, int j) {
    if (j < static_cast<int>(labels.size())) return labels[j];
    return "regime_" + std::to_string(j);
}

static void print_transition_matrix(const MSDFMOutput& out) {
    const int J = out.J;
    std::cout << "\n===== TRANSITION MATRIX (P) =====\n";
    std::cout << std::left << std::setw(20) << "From \\ To";
    for (int j = 0; j < J; ++j)
        std::cout << std::right << std::setw(18) << regime_label(out.regime_labels, j);
    std::cout << "\n" << std::string(20 + J * 18, '-') << "\n";

    for (int i = 0; i < J; ++i) {
        std::cout << std::left << std::setw(20) << regime_label(out.regime_labels, i);
        for (int j = 0; j < J; ++j) {
            std::cout << std::right << std::setw(18) << std::fixed
                      << std::setprecision(4) << out.transition_matrix(i, j);
        }
        std::cout << "\n";
    }
}

static void print_ergodic(const MSDFMOutput& out) {
    std::cout << "\n===== ERGODIC (STATIONARY) DISTRIBUTION =====\n";
    for (int j = 0; j < out.J; ++j) {
        std::cout << "  " << std::left << std::setw(18)
                  << regime_label(out.regime_labels, j)
                  << std::fixed << std::setprecision(4) << out.ergodic_probs(j)
                  << " (" << std::setprecision(1) << (out.ergodic_probs(j) * 100) << "%)\n";
    }
}

static void print_regime_signatures(const MSDFMOutput& out) {
    const int J = out.J;
    const int K = out.K;

    std::cout << "\n===== REGIME SIGNATURES =====\n";
    for (int j = 0; j < J; ++j) {
        const auto& sig = out.regime_signatures[j];
        std::cout << "\n--- " << regime_label(out.regime_labels, j) << " ---\n";
        std::cout << "  Mean volatility (trace(Q)/K): " << std::fixed << std::setprecision(4)
                  << sig.mean_volatility << "\n";
        std::cout << "  A diagonal (persistence):     ";
        for (int k = 0; k < K; ++k)
            std::cout << std::setprecision(4) << sig.A(k, k) << "  ";
        std::cout << "\n";
        std::cout << "  Mean factor levels:           ";
        for (int k = 0; k < K; ++k)
            std::cout << std::setprecision(4) << sig.mean_factors(k) << "  ";
        std::cout << "\n";
    }
}

static void print_regime_probs(const MSDFMOutput& out,
                                const std::vector<std::string>& dates,
                                int n_dates = 20) {
    const int T = out.T;
    const int J = out.J;

    std::cout << "\n===== REGIME PROBABILITIES (last " << n_dates << " dates) =====\n";
    std::cout << std::left << std::setw(14) << "Date";
    for (int j = 0; j < J; ++j)
        std::cout << std::right << std::setw(18) << regime_label(out.regime_labels, j);
    std::cout << std::setw(14) << "Regime";
    std::cout << "\n" << std::string(14 + J * 18 + 14, '-') << "\n";

    int start = std::max(0, T - n_dates);
    for (int t = start; t < T; ++t) {
        std::cout << std::left << std::setw(14) << dates[t];
        for (int j = 0; j < J; ++j) {
            std::cout << std::right << std::setw(18) << std::fixed
                      << std::setprecision(4) << out.smoothed_probs(t, j);
        }
        std::cout << std::setw(14) << regime_label(out.regime_labels, out.decoded_regimes[t]);
        std::cout << "\n";
    }
}

static void export_regimes_csv(const MSDFMOutput& out,
                                const std::vector<std::string>& dates,
                                const std::string& output_path) {
    std::ofstream f(output_path);
    if (!f.is_open()) {
        std::cerr << "[WARN] Cannot write to " << output_path << "\n";
        return;
    }

    f << "date";
    for (int j = 0; j < out.J; ++j)
        f << ",prob_" << regime_label(out.regime_labels, j);
    f << ",decoded_regime\n";

    for (int t = 0; t < out.T; ++t) {
        f << dates[t];
        for (int j = 0; j < out.J; ++j)
            f << "," << std::fixed << std::setprecision(6) << out.smoothed_probs(t, j);
        f << "," << regime_label(out.regime_labels, out.decoded_regimes[t]);
        f << "\n";
    }

    std::cout << "[INFO] Regime probabilities exported to: " << output_path << "\n";
}

// ============================================================================
// main
// ============================================================================

int main(int argc, char* argv[]) {

    // Parse args
    std::string conn_string;
    const char* env_conn = std::getenv("MACRO_DB_CONN");
    if (argc >= 2) {
        conn_string = argv[1];
    } else if (env_conn) {
        conn_string = env_conn;
    } else {
        conn_string = "dbname=new_algo_data";
    }

    std::string output_csv = (argc >= 3) ? argv[2] : "";
    std::string start_date = (argc >= 4) ? argv[3] : "";
    std::string end_date   = (argc >= 5) ? argv[4] : "";

    std::cout << "========================================\n"
              << " Macro MS-DFM Runner (PostgreSQL)\n"
              << "========================================\n\n"
              << "Connection: " << conn_string << "\n";
    if (!start_date.empty()) std::cout << "Start date: " << start_date << "\n";
    if (!end_date.empty())   std::cout << "End date:   " << end_date << "\n";
    std::cout << "\n";

    // Connect
    PostgresDatabase db(conn_string);
    auto conn_result = db.connect();
    if (conn_result.is_error()) {
        std::cerr << "[ERROR] DB connection failed: " << conn_result.error()->what() << "\n";
        return 1;
    }
    std::cout << "[OK] Connected to database\n\n";

    // Load macro panel
    MacroDataLoader loader(db);
    auto panel_result = loader.load(start_date, end_date);
    if (panel_result.is_error()) {
        std::cerr << "[ERROR] Failed to load macro data: "
                  << panel_result.error()->what() << "\n";
        return 1;
    }
    auto& panel = panel_result.value();
    std::cout << "[OK] Loaded panel: " << panel.T << " dates x " << panel.N << " series\n\n";

    // Step 1: Fit DFM
    DFMConfig dfm_config;
    dfm_config.num_factors       = 3;
    dfm_config.factor_labels     = {"macro_level", "real_activity", "commodity_inflation"};
    dfm_config.max_em_iterations = 200;
    dfm_config.em_tol            = 1e-6;
    dfm_config.standardise_data  = true;

    DynamicFactorModel dfm(dfm_config);
    std::cout << "Fitting DFM (B1)...\n";
    auto dfm_result = dfm.fit(panel.data, panel.column_names);
    if (dfm_result.is_error()) {
        std::cerr << "[ERROR] DFM fit failed: " << dfm_result.error()->what() << "\n";
        return 1;
    }
    const auto& dfm_out = dfm_result.value();
    std::cout << "[OK] DFM: " << (dfm_out.converged ? "converged" : "not converged")
              << " after " << dfm_out.em_iterations << " iters"
              << " (K=" << dfm_out.K << ")\n\n";

    // Step 2: Fit MS-DFM
    MSDFMConfig ms_config;
    ms_config.n_regimes         = 3;
    ms_config.regime_labels     = {"expansion", "slowdown", "stress"};
    ms_config.max_em_iterations = 200;
    ms_config.em_tol            = 1e-5;

    MarkovSwitchingDFM msdfm(ms_config);
    std::cout << "Fitting MS-DFM (B2) with " << ms_config.n_regimes << " regimes...\n";
    auto ms_result = msdfm.fit(dfm_out);
    if (ms_result.is_error()) {
        std::cerr << "[ERROR] MS-DFM fit failed: " << ms_result.error()->what() << "\n";
        return 1;
    }
    const auto& ms_out = ms_result.value();

    // Print results
    std::cout << "\n===== MS-DFM FIT SUMMARY =====\n"
              << "Converged:       " << (ms_out.convergence_info.converged ? "YES" : "NO") << "\n"
              << "EM iterations:   " << ms_out.convergence_info.iterations << "\n"
              << "Log-likelihood:  " << std::fixed << std::setprecision(2)
              << ms_out.log_likelihood << "\n"
              << "Final |dLL|:     " << std::scientific << std::setprecision(2)
              << ms_out.convergence_info.final_tolerance << "\n"
              << "Dimensions:      T=" << ms_out.T << " K=" << ms_out.K
              << " J=" << ms_out.J << "\n";

    print_transition_matrix(ms_out);
    print_ergodic(ms_out);
    print_regime_signatures(ms_out);
    print_regime_probs(ms_out, panel.dates);

    // Current regime
    int current = ms_out.decoded_regimes.back();
    std::cout << "\n===== CURRENT MACRO REGIME =====\n"
              << "  " << regime_label(ms_out.regime_labels, current)
              << " (as of " << panel.dates.back() << ")\n"
              << "  Probabilities: ";
    for (int j = 0; j < ms_out.J; ++j) {
        std::cout << regime_label(ms_out.regime_labels, j) << "="
                  << std::fixed << std::setprecision(3)
                  << ms_out.smoothed_probs(ms_out.T - 1, j) << "  ";
    }
    std::cout << "\n";

    // Export
    if (!output_csv.empty()) {
        export_regimes_csv(ms_out, panel.dates, output_csv);
    }

    return 0;
}
