// macro_dfm_runner.cpp
// Loads macro panel from PostgreSQL (new_algo_data.macro_data) → runs DFM
// → prints factor time series + loadings
//
// Usage:
//   ./macro_dfm_runner [connection_string] [output.csv] [start_date] [end_date]
//
// Defaults:
//   connection_string: $MACRO_DB_CONN or "dbname=new_algo_data"
//   output.csv:        (none — prints to stdout only)
//   start/end_date:    (all available data)

#include "trade_ngin/statistics/state_estimation/dynamic_factor_model.hpp"
#include "trade_ngin/statistics/state_estimation/macro_data_loader.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/core/logger.hpp"

#include <Eigen/Dense>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <cmath>
#include <cstdlib>

using namespace trade_ngin;
using namespace trade_ngin::statistics;

// ============================================================================
// Print helpers
// ============================================================================

// Get ordered label for factor k using the config's factor_labels
static std::string factor_label(const std::vector<std::string>& labels, int k) {
    if (k < static_cast<int>(labels.size())) return labels[k];
    return "f" + std::to_string(k);
}

static void print_loadings(const DFMOutput& out,
                           const std::vector<std::string>& labels) {
    const int N = out.N;
    const int K = out.K;

    std::cout << "\n===== FACTOR LOADINGS (Lambda) =====\n";
    std::cout << std::left << std::setw(30) << "Series";
    for (int k = 0; k < K; ++k) {
        std::cout << std::right << std::setw(22) << factor_label(labels, k);
    }
    std::cout << "\n" << std::string(30 + K * 22, '-') << "\n";

    for (int n = 0; n < N; ++n) {
        std::cout << std::left << std::setw(30) << out.series_names[n];
        for (int k = 0; k < K; ++k) {
            std::cout << std::right << std::setw(22) << std::fixed
                      << std::setprecision(4) << out.lambda(n, k);
        }
        std::cout << "\n";
    }
}

static void print_factor_summary(const DFMOutput& out,
                                  const std::vector<std::string>& dates,
                                  const std::vector<std::string>& labels) {
    const int T = out.T;
    const int K = out.K;

    std::cout << "\n===== FACTOR TIME SERIES (last 20 dates) =====\n";
    std::cout << std::left << std::setw(14) << "Date";
    for (int k = 0; k < K; ++k) {
        std::cout << std::right << std::setw(20) << factor_label(labels, k)
                  << std::setw(10) << "(+/-)";
    }
    std::cout << "\n" << std::string(14 + K * 24, '-') << "\n";

    int start = std::max(0, T - 20);
    for (int t = start; t < T; ++t) {
        std::cout << std::left << std::setw(14) << dates[t];
        for (int k = 0; k < K; ++k) {
            std::cout << std::right << std::setw(14) << std::fixed
                      << std::setprecision(4) << out.factors[t][k]
                      << std::setw(10) << std::setprecision(4)
                      << out.factor_uncertainty[t][k];
        }
        std::cout << "\n";
    }
}

static void print_transition_matrix(const DFMOutput& out) {
    const int K = out.K;
    std::cout << "\n===== FACTOR TRANSITION MATRIX (A) =====\n";
    for (int i = 0; i < K; ++i) {
        for (int j = 0; j < K; ++j) {
            std::cout << std::setw(10) << std::fixed << std::setprecision(4) << out.A(i, j);
        }
        std::cout << "\n";
    }

    std::cout << "\nDiagonal (persistence): ";
    for (int k = 0; k < K; ++k)
        std::cout << std::fixed << std::setprecision(4) << out.A(k, k) << "  ";
    std::cout << "\n";
}

static void export_factors_csv(const DFMOutput& out,
                                const std::vector<std::string>& dates,
                                const std::vector<std::string>& labels,
                                const std::string& output_path) {
    std::ofstream f(output_path);
    if (!f.is_open()) {
        std::cerr << "[WARN] Cannot write to " << output_path << "\n";
        return;
    }

    f << "date";
    for (int k = 0; k < out.K; ++k) {
        std::string lbl = factor_label(labels, k);
        f << "," << lbl << "," << lbl << "_uncertainty";
    }
    f << "\n";

    for (int t = 0; t < out.T; ++t) {
        f << dates[t];
        for (int k = 0; k < out.K; ++k) {
            f << "," << std::fixed << std::setprecision(6)
              << out.factors[t][k]
              << "," << out.factor_uncertainty[t][k];
        }
        f << "\n";
    }

    std::cout << "[INFO] Factor series exported to: " << output_path << "\n";
}

// ============================================================================
// main
// ============================================================================

int main(int argc, char* argv[]) {

    // ---- Parse args ----------------------------------------------------------
    // Default connection string: env var or local new_algo_data
    std::string conn_string;
    const char* env_conn = std::getenv("MACRO_DB_CONN");
    if (argc >= 2) {
        conn_string = argv[1];
    } else if (env_conn) {
        conn_string = env_conn;
    } else {
        conn_string = "dbname=new_algo_data";
    }

    std::string output_csv  = (argc >= 3) ? argv[2] : "";
    std::string start_date  = (argc >= 4) ? argv[3] : "";
    std::string end_date    = (argc >= 5) ? argv[4] : "";

    std::cout << "========================================\n"
              << " Macro DFM Runner (PostgreSQL)\n"
              << "========================================\n\n"
              << "Connection: " << conn_string << "\n";
    if (!start_date.empty()) std::cout << "Start date: " << start_date << "\n";
    if (!end_date.empty())   std::cout << "End date:   " << end_date << "\n";
    std::cout << "\n";

    // ---- Connect to database -------------------------------------------------
    PostgresDatabase db(conn_string);
    auto conn_result = db.connect();
    if (conn_result.is_error()) {
        std::cerr << "[ERROR] DB connection failed: " << conn_result.error()->what() << "\n";
        return 1;
    }
    std::cout << "[OK] Connected to database\n\n";

    // ---- Load macro panel ----------------------------------------------------
    MacroDataLoader loader(db);
    auto panel_result = loader.load(start_date, end_date);
    if (panel_result.is_error()) {
        std::cerr << "[ERROR] Failed to load macro data: "
                  << panel_result.error()->what() << "\n";
        return 1;
    }

    auto& panel = panel_result.value();
    std::cout << "[OK] Loaded panel: " << panel.T << " dates x " << panel.N << " series\n";
    std::cout << "     Date range: " << panel.dates.front()
              << " to " << panel.dates.back() << "\n";
    std::cout << "     Columns: ";
    for (int i = 0; i < panel.N; ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << panel.column_names[i];
    }
    std::cout << "\n\n";

    // ---- Configure DFM -------------------------------------------------------
    DFMConfig config;
    config.num_factors       = 3;
    config.factor_labels     = {"macro_level", "real_activity", "commodity_inflation"};
    config.max_em_iterations = 200;
    config.em_tol            = 1e-6;
    config.standardise_data  = true;

    if (panel.N < config.num_factors) {
        config.num_factors = panel.N;
        config.factor_labels.resize(config.num_factors);
        std::cerr << "[WARN] Only " << panel.N
                  << " series, reducing to " << config.num_factors << " factors\n";
    }

    std::cout << "DFM config: K=" << config.num_factors
              << " factors, max_iter=" << config.max_em_iterations
              << ", tol=" << config.em_tol << "\n\n";

    // ---- Fit DFM -------------------------------------------------------------
    DynamicFactorModel dfm(config);

    std::cout << "Fitting DFM...\n";
    auto result = dfm.fit(panel.data, panel.column_names);

    if (result.is_error()) {
        std::cerr << "[ERROR] DFM fit failed: " << result.error()->what() << "\n";
        return 1;
    }

    const auto& out = result.value();

    // ---- Print results -------------------------------------------------------
    std::cout << "\n===== FIT SUMMARY =====\n"
              << "Converged:       " << (out.converged ? "YES" : "NO") << "\n"
              << "EM iterations:   " << out.em_iterations << "\n"
              << "Log-likelihood:  " << std::fixed << std::setprecision(2)
              << out.log_likelihood << "\n"
              << "Final |dLL|:     " << std::scientific << std::setprecision(2)
              << out.final_ll_delta << "\n"
              << "Dimensions:      T=" << out.T << " N=" << out.N << " K=" << out.K << "\n";

    print_loadings(out, config.factor_labels);
    print_transition_matrix(out);
    print_factor_summary(out, panel.dates, config.factor_labels);

    std::cout << "\n===== FACTOR LABEL VALIDATION =====\n"
              << "Check the loadings above to verify that:\n"
              << "  - Factor 0 ('macro_level')          loads broadly on: GDP, CPI, payrolls, retail, M2, rates\n"
              << "  - Factor 1 ('real_activity')         loads on: capacity util, industrial prod, unemployment, VIX\n"
              << "  - Factor 2 ('commodity_inflation')   loads on: WTI crude, breakevens, capacity util\n"
              << "If the ordering doesn't match, relabel the factors accordingly.\n";

    // ---- Export if requested -------------------------------------------------
    if (!output_csv.empty()) {
        export_factors_csv(out, panel.dates, config.factor_labels, output_csv);
    }

    return 0;
}
