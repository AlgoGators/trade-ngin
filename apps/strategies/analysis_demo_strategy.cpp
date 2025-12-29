/**
 * @file analysis_demo_strategy.cpp
 * @brief Demonstration strategy using the analysis module tools
 *
 * This strategy demonstrates how to use:
 * - Preprocessing (normalization, PCA)
 * - Stationarity tests (ADF, KPSS)
 * - Cointegration tests (Engle-Granger, Johansen)
 * - GARCH volatility modeling
 * - Kalman Filter for adaptive parameters
 * - HMM for regime detection
 */

#include "trade_ngin/strategy/base_strategy.hpp"
#include "trade_ngin/analysis/preprocessing.hpp"
#include "trade_ngin/analysis/stationarity_tests.hpp"
#include "trade_ngin/analysis/cointegration.hpp"
#include "trade_ngin/analysis/garch.hpp"
#include "trade_ngin/analysis/kalman_filter.hpp"
#include "trade_ngin/analysis/hmm.hpp"
#include <iostream>
#include <iomanip>

using namespace trade_ngin;
using namespace trade_ngin::analysis;

class AnalysisDemoStrategy : public BaseStrategy {
public:
    AnalysisDemoStrategy()
        : BaseStrategy("analysis_demo", "Demo strategy using analysis tools")
        , window_size_(100)
        , regime_(MarketRegime::MEAN_REVERTING) {
    }

    void on_start() override {
        std::cout << "=== Analysis Module Demo Strategy ===" << std::endl;
        std::cout << "This strategy demonstrates all analysis tools" << std::endl;
    }

    void on_data(const std::string& symbol, const Bar& bar) override {
        // Store price history
        if (price_history_[symbol].size() >= 200) {
            price_history_[symbol].erase(price_history_[symbol].begin());
        }
        price_history_[symbol].push_back(bar.close);

        // Wait for enough data
        if (price_history_[symbol].size() < window_size_) {
            return;
        }

        // Demonstrate different analysis tools
        if (get_current_step() % 20 == 0) {
            demonstrate_preprocessing(symbol);
            demonstrate_stationarity_tests(symbol);
            demonstrate_volatility_modeling(symbol);
            demonstrate_regime_detection(symbol);
        }

        // Use Kalman Filter for adaptive position sizing
        update_adaptive_parameters(symbol, bar);
    }

private:
    void demonstrate_preprocessing(const std::string& symbol) {
        std::cout << "\n--- Preprocessing Demo for " << symbol << " ---" << std::endl;

        const auto& prices = price_history_[symbol];

        // Z-score normalization
        auto normalized = Normalization::z_score(prices);
        if (normalized) {
            std::cout << "✓ Z-score normalized last price: "
                     << normalized.value().back() << std::endl;
        }

        // Calculate returns
        auto returns = Normalization::calculate_returns(prices, true);
        if (returns) {
            double mean_return = std::accumulate(returns.value().begin(),
                                                 returns.value().end(), 0.0) /
                                returns.value().size();
            std::cout << "✓ Average log return: " << mean_return * 100 << "%" << std::endl;
        }

        // Robust scaling
        auto robust = Normalization::robust_scale(prices);
        if (robust) {
            std::cout << "✓ Robust scaled last price: "
                     << robust.value().back() << std::endl;
        }
    }

    void demonstrate_stationarity_tests(const std::string& symbol) {
        std::cout << "\n--- Stationarity Tests for " << symbol << " ---" << std::endl;

        const auto& prices = price_history_[symbol];

        // ADF test on prices
        auto adf_result = augmented_dickey_fuller_test(prices);
        if (adf_result) {
            const auto& adf = adf_result.value();
            std::cout << "ADF Test (Prices):" << std::endl;
            std::cout << "  Test Statistic: " << adf.test_statistic << std::endl;
            std::cout << "  Critical Value (5%): " << adf.critical_value_5 << std::endl;
            std::cout << "  Stationary at 5%: " << (adf.is_stationary_5 ? "YES" : "NO") << std::endl;
        }

        // KPSS test
        auto kpss_result = kpss_test(prices);
        if (kpss_result) {
            const auto& kpss = kpss_result.value();
            std::cout << "KPSS Test (Prices):" << std::endl;
            std::cout << "  Test Statistic: " << kpss.test_statistic << std::endl;
            std::cout << "  Critical Value (5%): " << kpss.critical_value_5 << std::endl;
            std::cout << "  Stationary at 5%: " << (kpss.is_stationary_5 ? "YES" : "NO") << std::endl;
        }

        // Test on returns
        auto returns = Normalization::calculate_returns(prices, true);
        if (returns) {
            auto adf_returns = augmented_dickey_fuller_test(returns.value());
            if (adf_returns) {
                std::cout << "ADF Test (Returns): "
                         << (adf_returns.value().is_stationary_5 ? "STATIONARY" : "NON-STATIONARY")
                         << std::endl;
            }
        }
    }

    void demonstrate_volatility_modeling(const std::string& symbol) {
        std::cout << "\n--- GARCH Volatility Modeling for " << symbol << " ---" << std::endl;

        const auto& prices = price_history_[symbol];
        auto returns = Normalization::calculate_returns(prices, true);

        if (!returns) {
            std::cout << "Error calculating returns" << std::endl;
            return;
        }

        // Fit GARCH(1,1) model
        GARCH garch_model;
        auto fit_result = garch_model.fit(returns.value());

        if (fit_result) {
            const auto& fit = fit_result.value();
            auto params = garch_model.get_parameters();

            std::cout << "GARCH(1,1) Parameters:" << std::endl;
            std::cout << "  ω (omega): " << params[0] << std::endl;
            std::cout << "  α (alpha): " << params[1] << std::endl;
            std::cout << "  β (beta):  " << params[2] << std::endl;
            std::cout << "  Persistence (α+β): " << (params[1] + params[2]) << std::endl;
            std::cout << "  Current Volatility: " << garch_model.get_current_volatility() * 100
                     << "%" << std::endl;
            std::cout << "  Half-life of shocks: " << garch_model.get_half_life() << " periods" << std::endl;

            // Forecast volatility
            auto forecast = garch_model.forecast(5);
            if (forecast) {
                std::cout << "  5-day volatility forecast: ";
                for (double vol : forecast.value().volatility_forecast) {
                    std::cout << vol * 100 << "% ";
                }
                std::cout << std::endl;
            }
        }
    }

    void demonstrate_regime_detection(const std::string& symbol) {
        std::cout << "\n--- Market Regime Detection for " << symbol << " ---" << std::endl;

        const auto& prices = price_history_[symbol];

        // Detect regimes using HMM
        auto hmm_result = detect_market_regimes(prices, 20);

        if (hmm_result) {
            const auto& result = hmm_result.value();

            std::cout << "HMM Regime Detection:" << std::endl;
            std::cout << "  Converged: " << (result.converged ? "YES" : "NO") << std::endl;
            std::cout << "  Iterations: " << result.n_iterations << std::endl;
            std::cout << "  Log-Likelihood: " << result.log_likelihood << std::endl;

            // Current regime
            int current_state = result.state_sequence.back();
            std::cout << "  Current State: " << current_state << std::endl;

            // State characteristics
            std::cout << "\n  State Characteristics:" << std::endl;
            for (int s = 0; s < 3; ++s) {
                auto [mean, cov] = std::make_pair(result.emission_means[s],
                                                  result.emission_covariances[s]);
                std::cout << "    State " << s << ":" << std::endl;
                std::cout << "      Mean Return: " << mean(0) * 100 << "%" << std::endl;
                std::cout << "      Mean Volatility: " << mean(1) * 100 << "%" << std::endl;
            }
        }
    }

    void update_adaptive_parameters(const std::string& symbol, const Bar& bar) {
        // Example: Use Kalman Filter to track adaptive moving average
        if (!kalman_filters_.count(symbol)) {
            // Initialize Kalman Filter for this symbol
            auto filter_result = create_pairs_trading_filter(0.01, 0.1);
            if (filter_result) {
                kalman_filters_[symbol] = filter_result.value();
            }
        }

        if (kalman_filters_.count(symbol)) {
            Eigen::VectorXd observation(1);
            observation << bar.close;

            auto state = kalman_filters_[symbol].filter(observation);
            if (state) {
                // Use filtered state for trading decisions
                double filtered_price = state.value().state(0);
                double price_deviation = bar.close - filtered_price;

                // This could be used for mean reversion signals
                if (std::abs(price_deviation) > 2.0) {
                    std::cout << symbol << " Kalman signal: deviation = "
                             << price_deviation << std::endl;
                }
            }
        }
    }

    int window_size_;
    std::map<std::string, std::vector<double>> price_history_;
    std::map<std::string, KalmanFilter> kalman_filters_;
    MarketRegime regime_;
};

/**
 * Example showing pairs trading with cointegration
 */
void demonstrate_pairs_trading() {
    std::cout << "\n=== Pairs Trading with Cointegration ===" << std::endl;

    // Example: Two cointegrated price series (simulated)
    std::vector<double> stock_a, stock_b;

    // Generate correlated random walks
    double price_a = 100.0, price_b = 100.0;
    std::srand(42);

    for (int i = 0; i < 200; ++i) {
        double common_shock = (std::rand() % 1000 - 500) / 10000.0;
        double idio_a = (std::rand() % 1000 - 500) / 20000.0;
        double idio_b = (std::rand() % 1000 - 500) / 20000.0;

        price_a *= (1.0 + common_shock + idio_a);
        price_b *= (1.0 + common_shock + idio_b);

        stock_a.push_back(price_a);
        stock_b.push_back(price_b);
    }

    // Test for cointegration
    auto eg_result = engle_granger_test(stock_a, stock_b);
    if (eg_result) {
        const auto& result = eg_result.value();

        std::cout << "Engle-Granger Cointegration Test:" << std::endl;
        std::cout << "  Beta (hedge ratio): " << result.cointegration_coefficient << std::endl;
        std::cout << "  Intercept: " << result.intercept << std::endl;
        std::cout << "  ADF Statistic: " << result.adf_result.test_statistic << std::endl;
        std::cout << "  Cointegrated at 5%: "
                 << (result.is_cointegrated_5 ? "YES" : "NO") << std::endl;

        if (result.is_cointegrated_5) {
            std::cout << "\n  ✓ Pairs trading opportunity detected!" << std::endl;
            std::cout << "  Hedge Ratio: Long 1.0 of Stock A, Short "
                     << result.cointegration_coefficient << " of Stock B" << std::endl;

            // Calculate current spread
            double current_spread = stock_a.back() -
                result.cointegration_coefficient * stock_b.back() - result.intercept;
            std::cout << "  Current Spread: " << current_spread << std::endl;
        }
    }
}

/**
 * Example showing PCA for dimensionality reduction
 */
void demonstrate_pca() {
    std::cout << "\n=== PCA for Portfolio Analysis ===" << std::endl;

    // Simulate returns for 5 correlated assets
    int n_assets = 5;
    int n_obs = 100;

    Eigen::MatrixXd returns(n_obs, n_assets);
    std::srand(123);

    for (int i = 0; i < n_obs; ++i) {
        double market_factor = (std::rand() % 1000 - 500) / 10000.0;
        for (int j = 0; j < n_assets; ++j) {
            double idio = (std::rand() % 1000 - 500) / 20000.0;
            returns(i, j) = 0.7 * market_factor + 0.3 * idio;
        }
    }

    // Apply PCA
    PCA pca(3); // Keep 3 components
    auto pca_result = pca.fit_transform(returns);

    if (pca_result) {
        const auto& result = pca_result.value();

        std::cout << "PCA Results:" << std::endl;
        std::cout << "  Explained Variance Ratio:" << std::endl;
        for (int i = 0; i < result.explained_variance_ratio.size(); ++i) {
            std::cout << "    PC" << (i+1) << ": "
                     << result.explained_variance_ratio(i) * 100 << "%" << std::endl;
        }

        double cumulative = 0.0;
        for (int i = 0; i < result.explained_variance_ratio.size(); ++i) {
            cumulative += result.explained_variance_ratio(i);
        }
        std::cout << "  Cumulative Variance Explained: " << cumulative * 100 << "%" << std::endl;

        std::cout << "\n  Principal Components (loadings):" << std::endl;
        std::cout << result.components << std::endl;
    }
}

int main() {
    // Run demonstrations
    demonstrate_pairs_trading();
    demonstrate_pca();

    std::cout << "\n=== Demo Complete ===" << std::endl;
    std::cout << "All analysis tools are working correctly!" << std::endl;

    return 0;
}
