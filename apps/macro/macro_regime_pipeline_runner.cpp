// macro_regime_pipeline_runner.cpp
// Synthesized Macro Regime Pipeline — fully automated.
// Runs all 4 models (DFM, MS-DFM, BSTS, Quadrant) from the database,
// combines into a single MacroBelief.
//
// Usage:
//   ./macro_regime_pipeline_runner [connection_string] [start_date] [end_date]

#include "trade_ngin/regime_detection/macro/macro_regime_pipeline.hpp"
#include "trade_ngin/regime_detection/macro/bsts_regime_detector.hpp"
#include "trade_ngin/regime_detection/macro/dynamic_factor_model.hpp"
#include "trade_ngin/regime_detection/macro/ms_dfm.hpp"
#include "trade_ngin/regime_detection/macro/macro_data_loader.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/core/logger.hpp"

#include <Eigen/Dense>
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <cstdlib>
#include <cmath>

using namespace trade_ngin;
using namespace trade_ngin::statistics;

// ============================================================================
// Print helpers
// ============================================================================

static void print_belief(const MacroBelief& b) {
    std::cout << "\n========================================================\n"
              << " MACRO REGIME BELIEF (Synthesized Pipeline)\n"
              << "========================================================\n\n";

    std::cout << "  Dominant regime: " << macro_regime_name(b.most_likely) << "\n";
    std::cout << "  Confidence:      " << std::fixed << std::setprecision(3)
              << b.confidence << "\n";
    std::cout << "  Regime age:      " << b.regime_age_bars << " periods\n\n";

    std::cout << "  REGIME PROBABILITIES\n";
    std::cout << "  ----------------------------------------\n";
    for (int i = 0; i < kNumMacroRegimes; ++i) {
        auto r = static_cast<MacroRegimeL1>(i);
        auto it = b.macro_probs.find(r);
        double p = (it != b.macro_probs.end()) ? it->second : 0.0;
        std::cout << "  " << std::left << std::setw(30) << macro_regime_name(r)
                  << std::fixed << std::setprecision(4) << p << "  ";
        int bar = static_cast<int>(p * 40);
        for (int j = 0; j < bar; ++j) std::cout << "#";
        std::cout << "\n";
    }

    std::cout << "\n  MODEL CONTRIBUTIONS\n";
    std::cout << "  ----------------------------------------\n";
    for (const auto& [model, probs] : b.model_contributions) {
        MacroRegimeL1 dom = MacroRegimeL1::EXPANSION_DISINFLATION;
        double max_p = 0;
        for (const auto& [r, p] : probs) {
            if (p > max_p) { max_p = p; dom = r; }
        }
        std::cout << "  " << std::left << std::setw(12) << model
                  << " -> " << macro_regime_name(dom)
                  << " (" << std::fixed << std::setprecision(3) << max_p << ")\n";
    }
    std::cout << "\n";
}

// ============================================================================
// main
// ============================================================================

int main(int argc, char* argv[]) {
    try {
        std::string conn_string;
        const char* env_conn = std::getenv("MACRO_DB_CONN");
        if (argc >= 2)      conn_string = argv[1];
        else if (env_conn)  conn_string = env_conn;
        else                conn_string = "dbname=new_algo_data";

        std::string start_date = (argc >= 3) ? argv[2] : "";
        std::string end_date   = (argc >= 4) ? argv[3] : "";

        std::cout << "========================================================\n"
                  << " Synthesized Macro Regime Pipeline\n"
                  << " B1(DFM) + B2(MS-DFM) + B3(Quadrant) + B4(BSTS/GMM)\n"
                  << " All models run automatically from database.\n"
                  << "========================================================\n\n";

        // ---- Step 1: Connect and load macro panel ---------------------------
        std::cout << "[1/5] Connecting to database...\n";
        PostgresDatabase db(conn_string);
        auto conn_result = db.connect();
        if (conn_result.is_error()) {
            std::cerr << "[ERROR] DB connection failed: "
                      << conn_result.error()->what() << "\n";
            return 1;
        }

        MacroDataLoader loader(db);
        auto panel_result = loader.load(start_date, end_date);
        if (panel_result.is_error()) {
            std::cerr << "[ERROR] Macro panel load failed: "
                      << panel_result.error()->what() << "\n";
            return 1;
        }
        auto& panel = panel_result.value();
        std::cout << "      Panel: " << panel.T << " dates x " << panel.N << " series\n";
        std::cout << "      Range: " << panel.dates.front() << " to "
                  << panel.dates.back() << "\n\n";

        // ---- Step 2: Fit DFM (B1) ------------------------------------------
        std::cout << "[2/5] Fitting DFM (B1)...\n";
        MacroRegimePipelineConfig pipe_config;  // declare early to read dfm_max_em_iterations

        DFMConfig dfm_config;
        dfm_config.num_factors       = 3;
        dfm_config.factor_labels     = {"macro_level", "real_activity", "commodity_inflation"};
        dfm_config.max_em_iterations = pipe_config.dfm_max_em_iterations;
        // CALIBRATION NOTE: Spec default 1e-6 is too tight for a 24-series
        // macro panel — EM oscillates near the optimum (|ΔLL|~0.005) without
        // converging. 0.01 accepts near-convergence since regime classification
        // only uses factor percentiles, not precise factor values.
        dfm_config.em_tol            = 0.01;
        dfm_config.standardise_data  = true;

        DynamicFactorModel dfm(dfm_config);
        auto dfm_result = dfm.fit(panel.data, panel.column_names);
        if (dfm_result.is_error()) {
            std::cerr << "[ERROR] DFM fit failed: "
                      << dfm_result.error()->what() << "\n";
            return 1;
        }
        const auto& dfm_out = dfm_result.value();
        std::cout << "      " << (dfm_out.converged ? "Converged" : "Not converged")
                  << " after " << dfm_out.em_iterations << " iterations"
                  << " (LL=" << std::fixed << std::setprecision(1)
                  << dfm_out.log_likelihood << ")\n";

        // Print top 3 loadings per factor so the user can verify factor labels.
        // If factor 0 loads on GDP/payrolls/IP → it's growth.
        // If factor 1 loads on CPI/PCE/breakeven → it's inflation.
        // If the labels don't match, change growth_factor_idx/inflation_factor_idx.
        std::cout << "\n      DFM FACTOR LOADINGS (top 3 per factor):\n";
        for (int k = 0; k < dfm_out.K; ++k) {
            // Sort series by absolute loading on this factor
            std::vector<std::pair<double, int>> loads;
            for (int n = 0; n < dfm_out.N; ++n)
                loads.push_back({std::abs(dfm_out.lambda(n, k)), n});
            std::sort(loads.begin(), loads.end(), std::greater<>());

            std::string label = (k < static_cast<int>(dfm_config.factor_labels.size()))
                                ? dfm_config.factor_labels[k]
                                : "f" + std::to_string(k);
            std::cout << "      Factor " << k << " (" << label << "): ";
            for (int i = 0; i < std::min(3, static_cast<int>(loads.size())); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << dfm_out.series_names[loads[i].second]
                          << " (" << std::fixed << std::setprecision(3)
                          << dfm_out.lambda(loads[i].second, k) << ")";
            }
            std::cout << "\n";
        }
        std::cout << "\n";

        // ---- Step 3: Fit MS-DFM (B2) on DFM factors ------------------------
        std::cout << "[3/5] Fitting MS-DFM (B2)...\n";
        MSDFMConfig ms_config;
        ms_config.n_regimes         = 3;
        ms_config.regime_labels     = {"expansion", "slowdown", "stress"};
        ms_config.max_em_iterations = 200;
        ms_config.em_tol            = 1e-5;

        MarkovSwitchingDFM msdfm(ms_config);
        auto ms_result = msdfm.fit(dfm_out);
        if (ms_result.is_error()) {
            std::cerr << "[ERROR] MS-DFM fit failed: "
                      << ms_result.error()->what() << "\n";
            return 1;
        }
        const auto& ms_out = ms_result.value();
        std::cout << "      " << (ms_out.convergence_info.converged ? "Converged" : "Not converged")
                  << " after " << ms_out.convergence_info.iterations << " iterations"
                  << " (LL=" << std::fixed << std::setprecision(1)
                  << ms_out.log_likelihood << ")\n\n";

        // ---- Step 4: Fit BSTS (B4) directly from DB -------------------------
        std::cout << "[4/5] Fitting BSTS (B4)...\n";
        BSTSConfig bsts_config;  // default: 8 ETFs, 12 macro, PCA 12, GMM 4
        BSTSRegimeDetector bsts(bsts_config);
        auto bsts_result = bsts.fit_from_db(db, start_date, end_date);
        if (bsts_result.is_error()) {
            std::cerr << "[ERROR] BSTS fit failed: "
                      << bsts_result.error()->what() << "\n";
            return 1;
        }
        const auto& bsts_out = bsts_result.value();
        std::cout << "      T=" << bsts_out.T
                  << "  high-conviction: " << std::fixed << std::setprecision(1)
                  << bsts_out.high_conviction_pct << "%\n\n";

        // ---- Step 5: Train pipeline and run ---------------------------------
        std::cout << "[5/5] Training Synthesized MacroRegimePipeline...\n";

        MacroRegimePipeline pipeline(pipe_config);

        auto train_result = pipeline.train(dfm_out, ms_out, panel, bsts_out);
        if (train_result.is_error()) {
            std::cerr << "[ERROR] Pipeline training failed: "
                      << train_result.error()->what() << "\n";
            return 1;
        }

        // Run update() for each timestep
        const int T = panel.T;
        const int K_dfm = dfm_out.K;
        const int ms_T = ms_out.T;
        const int ms_offset = T - std::min(T, ms_T);

        // Structural break detection from BSTS entropy
        constexpr double kBreakEntropyThreshold = 0.20;
        int break_count = 0;

        std::vector<MacroBelief> beliefs(T);
        for (int t = 0; t < T; ++t) {
            // DFM factors
            Eigen::Vector3d f_t;
            for (int k = 0; k < std::min(K_dfm, 3); ++k)
                f_t(k) = dfm_out.factors[t][k];

            // MS-DFM native probs
            Eigen::Vector3d ms_t = Eigen::Vector3d::Constant(1.0 / 3.0);
            int ms_idx = t - ms_offset;
            if (ms_idx >= 0 && ms_idx < ms_T) {
                for (int j = 0; j < std::min(ms_out.J, 3); ++j)
                    ms_t(j) = ms_out.smoothed_probs(ms_idx, j);
            }

            // BSTS cluster probs + entropy
            Eigen::Vector4d b_t = Eigen::Vector4d::Constant(0.25);
            bool structural_break = false;
            if (t < bsts_out.T) {
                for (int k = 0; k < std::min(bsts_out.num_regimes, 4); ++k)
                    b_t(k) = bsts_out.regime_posteriors(t, k);

                // Break detection: high entropy or cluster change
                if (bsts_out.regime_entropy(t) > kBreakEntropyThreshold)
                    structural_break = true;
                if (t > 0) {
                    int prev_dom, curr_dom;
                    bsts_out.regime_posteriors.row(t - 1).maxCoeff(&prev_dom);
                    bsts_out.regime_posteriors.row(t).maxCoeff(&curr_dom);
                    if (prev_dom != curr_dom)
                        structural_break = true;
                }
            }
            if (structural_break) ++break_count;

            // Growth/inflation from BSTS composites
            double gs = (t < bsts_out.T) ? bsts_out.growth_score(t) : 0.0;
            double is = (t < bsts_out.T) ? bsts_out.inflation_score(t) : 0.0;

            auto belief_result = pipeline.update(
                f_t, ms_t, gs, is, b_t, structural_break);

            if (belief_result.is_ok())
                beliefs[t] = belief_result.value();
        }

        std::cout << "      Structural breaks detected: " << break_count
                  << "/" << T << " timesteps\n";

        // ---- Print final MacroBelief ----------------------------------------
        print_belief(beliefs.back());

        // ---- Regime history (last 20 dates) ---------------------------------
        std::cout << "  REGIME HISTORY (last 20 dates)\n";
        std::cout << "  ----------------------------------------\n";
        std::cout << "  " << std::left << std::setw(14) << "Date"
                  << std::setw(30) << "Regime"
                  << std::right << std::setw(10) << "Conf"
                  << std::setw(8) << "Age" << "\n";
        std::cout << "  " << std::string(62, '-') << "\n";

        int start = std::max(0, T - 20);
        for (int t = start; t < T; ++t) {
            std::cout << "  " << std::left << std::setw(14) << panel.dates[t]
                      << std::setw(30) << macro_regime_name(beliefs[t].most_likely)
                      << std::right << std::fixed << std::setprecision(3)
                      << std::setw(10) << beliefs[t].confidence
                      << std::setw(8) << beliefs[t].regime_age_bars << "\n";
        }

        // ---- DIAGNOSTIC: MS-DFM lock-in check ------------------------------
        // Purpose: verify whether MS-DFM's native state 0 (90% of sample)
        // produces a structural lock-in on SLOWDOWN_DISINFLATION, and whether
        // native states 1/2 cluster in later years (time-trend artifact from
        // using raw column levels in the fingerprint).

        std::cout << "\n  === DIAGNOSTIC: MS-DFM BEHAVIOUR BY YEAR ===\n";
        std::cout << "  ----------------------------------------------------------\n";
        std::cout << "  " << std::left << std::setw(6) << "Year"
                  << std::setw(7) << "N"
                  << std::setw(20) << "MS-DFM dominant"
                  << std::setw(8) << "ns0%"
                  << std::setw(8) << "ns1%"
                  << std::setw(8) << "ns2%"
                  << std::setw(22) << "Ensemble dominant" << "\n";
        std::cout << "  " << std::string(76, '-') << "\n";

        // Aggregate by year
        struct YearStats {
            int n = 0;
            std::vector<int> msdfm_pick{std::vector<int>(kNumMacroRegimes, 0)};
            std::vector<int> dfm_pick{std::vector<int>(kNumMacroRegimes, 0)};
            std::vector<int> quad_pick{std::vector<int>(kNumMacroRegimes, 0)};
            std::vector<int> bsts_pick{std::vector<int>(kNumMacroRegimes, 0)};
            std::vector<int> ensemble_pick{std::vector<int>(kNumMacroRegimes, 0)};
            std::vector<int> native_count{std::vector<int>(3, 0)};
            std::vector<int> bsts_cluster_count{std::vector<int>(4, 0)};
        };
        std::map<std::string, YearStats> by_year;

        auto pick_argmax = [](const std::map<MacroRegimeL1, double>& m) -> int {
            int best = 0; double best_p = 0.0;
            for (int r = 0; r < kNumMacroRegimes; ++r) {
                auto it = m.find(static_cast<MacroRegimeL1>(r));
                if (it != m.end() && it->second > best_p) {
                    best_p = it->second; best = r;
                }
            }
            return best;
        };

        for (int t = 0; t < T; ++t) {
            std::string year = panel.dates[t].substr(0, 4);
            auto& ys = by_year[year];
            ys.n++;

            // Per-model dominant regime from per-model contributions
            const auto& mc = beliefs[t].model_contributions;
            auto ms_it = mc.find("MS-DFM");
            auto df_it = mc.find("DFM");
            auto qd_it = mc.find("Quadrant");
            auto bs_it = mc.find("BSTS");
            if (ms_it != mc.end()) ys.msdfm_pick[pick_argmax(ms_it->second)]++;
            if (df_it != mc.end()) ys.dfm_pick  [pick_argmax(df_it->second)]++;
            if (qd_it != mc.end()) ys.quad_pick [pick_argmax(qd_it->second)]++;
            if (bs_it != mc.end()) ys.bsts_pick [pick_argmax(bs_it->second)]++;

            // Ensemble dominant
            ys.ensemble_pick[static_cast<int>(beliefs[t].most_likely)]++;

            // MS-DFM native state (argmax of smoothed probs)
            int ms_idx = t - ms_offset;
            if (ms_idx >= 0 && ms_idx < ms_T) {
                int best_ns = 0; double best_p = 0.0;
                for (int j = 0; j < std::min(ms_out.J, 3); ++j) {
                    double p = ms_out.smoothed_probs(ms_idx, j);
                    if (p > best_p) { best_p = p; best_ns = j; }
                }
                ys.native_count[best_ns]++;
            }

            // BSTS native cluster (argmax of regime posteriors)
            if (t < bsts_out.T) {
                int best_c = 0; double best_p = 0.0;
                for (int k = 0; k < std::min(bsts_out.num_regimes, 4); ++k) {
                    double p = bsts_out.regime_posteriors(t, k);
                    if (p > best_p) { best_p = p; best_c = k; }
                }
                ys.bsts_cluster_count[best_c]++;
            }
        }

        for (const auto& [year, ys] : by_year) {
            // Argmax picks
            int msdfm_best = 0, ens_best = 0;
            int msdfm_max = 0, ens_max = 0;
            for (int r = 0; r < kNumMacroRegimes; ++r) {
                if (ys.msdfm_pick[r] > msdfm_max)     { msdfm_max = ys.msdfm_pick[r]; msdfm_best = r; }
                if (ys.ensemble_pick[r] > ens_max)    { ens_max   = ys.ensemble_pick[r]; ens_best = r; }
            }
            double ns0 = 100.0 * ys.native_count[0] / std::max(1, ys.n);
            double ns1 = 100.0 * ys.native_count[1] / std::max(1, ys.n);
            double ns2 = 100.0 * ys.native_count[2] / std::max(1, ys.n);

            std::cout << "  " << std::left << std::setw(6) << year
                      << std::setw(7) << ys.n
                      << std::setw(20) << macro_regime_name(static_cast<MacroRegimeL1>(msdfm_best))
                      << std::right << std::fixed << std::setprecision(0)
                      << std::setw(6) << ns0 << "  "
                      << std::setw(6) << ns1 << "  "
                      << std::setw(6) << ns2 << "  "
                      << std::left << "  " << std::setw(20)
                      << macro_regime_name(static_cast<MacroRegimeL1>(ens_best))
                      << "\n";
        }

        // Full-sample MS-DFM pick breakdown
        std::vector<int> msdfm_total(kNumMacroRegimes, 0);
        for (const auto& [y, ys] : by_year)
            for (int r = 0; r < kNumMacroRegimes; ++r)
                msdfm_total[r] += ys.msdfm_pick[r];

        std::cout << "\n  MS-DFM dominant pick, full sample:\n";
        for (int r = 0; r < kNumMacroRegimes; ++r) {
            double pct = 100.0 * msdfm_total[r] / T;
            std::cout << "    " << std::left << std::setw(28)
                      << macro_regime_name(static_cast<MacroRegimeL1>(r))
                      << std::right << std::fixed << std::setprecision(1)
                      << std::setw(6) << pct << "%\n";
        }

        // ---- Per-year diagnostic tables for the other 3 models ---------------
        auto print_per_year_table = [&](const std::string& label,
            const std::vector<int> YearStats::*field) {
            std::cout << "\n  === DIAGNOSTIC: " << label << " BEHAVIOUR BY YEAR ===\n";
            std::cout << "  ----------------------------------------------------------\n";
            std::cout << "  " << std::left << std::setw(6) << "Year"
                      << std::setw(7) << "N"
                      << std::setw(28) << (label + " dominant")
                      << std::setw(22) << "Ensemble dominant" << "\n";
            std::cout << "  " << std::string(62, '-') << "\n";
            for (const auto& [year, ys] : by_year) {
                int model_best = 0, ens_best = 0;
                int model_max = 0, ens_max = 0;
                const auto& picks = ys.*field;
                for (int r = 0; r < kNumMacroRegimes; ++r) {
                    if (picks[r] > model_max) { model_max = picks[r]; model_best = r; }
                    if (ys.ensemble_pick[r] > ens_max) {
                        ens_max = ys.ensemble_pick[r]; ens_best = r;
                    }
                }
                std::cout << "  " << std::left << std::setw(6) << year
                          << std::setw(7) << ys.n
                          << std::setw(28) << macro_regime_name(static_cast<MacroRegimeL1>(model_best))
                          << std::setw(22) << macro_regime_name(static_cast<MacroRegimeL1>(ens_best))
                          << "\n";
            }
            // Full-sample distribution
            std::vector<int> total(kNumMacroRegimes, 0);
            for (const auto& [y, ys] : by_year)
                for (int r = 0; r < kNumMacroRegimes; ++r)
                    total[r] += (ys.*field)[r];
            std::cout << "\n  " << label << " dominant pick, full sample:\n";
            for (int r = 0; r < kNumMacroRegimes; ++r) {
                double pct = 100.0 * total[r] / T;
                std::cout << "    " << std::left << std::setw(28)
                          << macro_regime_name(static_cast<MacroRegimeL1>(r))
                          << std::right << std::fixed << std::setprecision(1)
                          << std::setw(6) << pct << "%\n";
            }
        };

        print_per_year_table("DFM",      &YearStats::dfm_pick);
        print_per_year_table("Quadrant", &YearStats::quad_pick);
        print_per_year_table("BSTS",     &YearStats::bsts_pick);

        // BSTS native cluster distribution by year (since BSTS has 4 native clusters)
        std::cout << "\n  === DIAGNOSTIC: BSTS NATIVE CLUSTER FREQ BY YEAR ===\n";
        std::cout << "  ----------------------------------------------------------\n";
        std::cout << "  " << std::left << std::setw(6) << "Year"
                  << std::setw(7) << "N"
                  << std::setw(8) << "c0%" << std::setw(8) << "c1%"
                  << std::setw(8) << "c2%" << std::setw(8) << "c3%" << "\n";
        std::cout << "  " << std::string(45, '-') << "\n";
        for (const auto& [year, ys] : by_year) {
            double pcts[4];
            for (int k = 0; k < 4; ++k)
                pcts[k] = 100.0 * ys.bsts_cluster_count[k] / std::max(1, ys.n);
            std::cout << "  " << std::left << std::setw(6) << year
                      << std::setw(7) << ys.n
                      << std::right << std::fixed << std::setprecision(0);
            for (int k = 0; k < 4; ++k)
                std::cout << std::setw(6) << pcts[k] << "  ";
            std::cout << "\n";
        }

        // ---- Regime distribution -------------------------------------------
        std::vector<int> counts(kNumMacroRegimes, 0);
        for (int t = 0; t < T; ++t)
            counts[static_cast<int>(beliefs[t].most_likely)]++;

        std::cout << "\n  REGIME DISTRIBUTION (full sample, T=" << T << ")\n";
        std::cout << "  ----------------------------------------\n";
        for (int i = 0; i < kNumMacroRegimes; ++i) {
            double pct = 100.0 * counts[i] / T;
            int bar = static_cast<int>(pct / 2.5);
            std::cout << "  " << std::left << std::setw(30)
                      << macro_regime_name(static_cast<MacroRegimeL1>(i))
                      << std::right << std::fixed << std::setprecision(1)
                      << std::setw(6) << pct << "%  ";
            for (int j = 0; j < bar; ++j) std::cout << "#";
            std::cout << "\n";
        }

        // ---- Model weights --------------------------------------------------
        // Reset precision: prior loops left std::setprecision(1) sticky on cout,
        // which truncated 0.25 to "0.2".
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "\n  MODEL WEIGHTS (Synthesized Spec)\n";
        std::cout << "  ----------------------------------------\n";
        std::cout << "  MS-DFM:   " << pipe_config.w_msdfm    << "\n";
        std::cout << "  DFM:      " << pipe_config.w_dfm      << "\n";
        std::cout << "  Quadrant: " << pipe_config.w_quadrant  << "\n";
        std::cout << "  BSTS:     " << pipe_config.w_bsts      << "\n";
        std::cout << "  Base lambda: " << pipe_config.base_lambda << "\n";
        std::cout << "  Min dwell:   " << pipe_config.min_dwell_bars << " bars\n";

        std::cout << "\nDone.\n";

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
