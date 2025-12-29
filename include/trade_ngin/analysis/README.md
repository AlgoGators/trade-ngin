# Trade Engine Analysis Module

This module provides a comprehensive suite of quantitative finance tools for backtesting and paper trading strategies. All tools are implemented in C++17 with minimal external dependencies (Eigen for linear algebra) and follow a plug-and-play design for easy integration into trading strategies.

## Table of Contents

1. [Overview](#overview)
2. [Installation](#installation)
3. [Module Components](#module-components)
4. [Usage Examples](#usage-examples)
5. [API Reference](#api-reference)

---

## Overview

The analysis module provides:

### Preprocessing & Feature Engineering
- **Data Normalization**: Z-score, min-max, robust scaling
- **PCA**: Dimensionality reduction for multi-asset portfolios
- **Returns Calculation**: Log returns and simple returns
- **Feature Extraction**: Helper functions for extracting prices from bars

### Statistical Testing
- **Stationarity Tests**:
  - Augmented Dickey-Fuller (ADF) test
  - KPSS test
  - Combined stationarity check

- **Cointegration Tests**:
  - Engle-Granger two-step method (pairwise)
  - Johansen test (multivariate)

### Volatility Modeling
- **GARCH(1,1)**: Conditional heteroskedasticity modeling
  - Parameter estimation via maximum likelihood
  - Volatility forecasting
  - Online updating for real-time trading

### State Estimation
- **Kalman Filter**: Dynamic state estimation
  - Linear state-space models
  - Adaptive parameter tracking
  - Smoothing algorithms (RTS)
  - Pre-configured filters for common use cases

### Regime Detection
- **Hidden Markov Models (HMM)**:
  - Gaussian emission HMM
  - Baum-Welch (EM) algorithm for fitting
  - Viterbi algorithm for state sequence decoding
  - Market regime classification

---

## Installation

### Prerequisites

1. **C++17 Compiler**: GCC 7+, Clang 5+, or MSVC 2017+
2. **CMake 3.17+**
3. **Eigen 3.4+**: Automatically included in `externals/eigen`

### Building

```bash
# Clone repository
cd trade-ngin

# Create build directory
mkdir build && cd build

# Configure
cmake .. -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build . --config Release
```

### Integration

Simply include the desired headers in your strategy:

```cpp
#include "trade_ngin/analysis/preprocessing.hpp"
#include "trade_ngin/analysis/stationarity_tests.hpp"
#include "trade_ngin/analysis/cointegration.hpp"
#include "trade_ngin/analysis/garch.hpp"
#include "trade_ngin/analysis/kalman_filter.hpp"
#include "trade_ngin/analysis/hmm.hpp"
```

---

## Module Components

### 1. Preprocessing (`preprocessing.hpp`)

#### Normalization Functions

**Z-Score Normalization**
```cpp
std::vector<double> prices = {100, 102, 101, 103, 105};
auto normalized = Normalization::z_score(prices);
// Result: mean=0, std=1
```

**Min-Max Normalization**
```cpp
auto normalized = Normalization::min_max(prices);
// Result: values in [0, 1]

auto custom_range = Normalization::min_max_range(prices, -1.0, 1.0);
// Result: values in [-1, 1]
```

**Robust Scaling**
```cpp
auto scaled = Normalization::robust_scale(prices);
// Uses median and IQR (robust to outliers)
```

#### Returns Calculation
```cpp
std::vector<double> prices = {100, 102, 101, 103};

// Log returns
auto log_returns = Normalization::calculate_returns(prices, true);

// Simple returns
auto simple_returns = Normalization::calculate_returns(prices, false);
```

#### Principal Component Analysis (PCA)

```cpp
// Multi-asset returns matrix (n_obs x n_assets)
Eigen::MatrixXd returns(100, 5);
// ... fill with data

// Create PCA model (keep 3 components)
PCA pca(3);

// Fit and transform
auto result = pca.fit_transform(returns);
if (result) {
    std::cout << "Explained variance: "
              << result.value().explained_variance_ratio << std::endl;

    // Transform new data
    auto transformed = pca.transform(new_data);

    // Inverse transform
    auto reconstructed = pca.inverse_transform(transformed.value());
}
```

---

### 2. Stationarity Tests (`stationarity_tests.hpp`)

#### Augmented Dickey-Fuller Test

```cpp
std::vector<double> prices = {/* your price series */};

// Run ADF test
auto adf_result = augmented_dickey_fuller_test(prices);

if (adf_result) {
    auto& result = adf_result.value();

    std::cout << "ADF Statistic: " << result.test_statistic << std::endl;
    std::cout << "Critical Value (5%): " << result.critical_value_5 << std::endl;
    std::cout << "Is Stationary: " << result.is_stationary_5 << std::endl;
    std::cout << "P-value: " << result.p_value << std::endl;
}

// Test on returns
auto returns = Normalization::calculate_returns(prices, true);
auto adf_returns = augmented_dickey_fuller_test(returns.value());
```

**Interpretation:**
- **Null Hypothesis**: Series has a unit root (non-stationary)
- **Rejection**: Test statistic < Critical value → Series is stationary
- More negative test statistics = stronger evidence for stationarity

#### KPSS Test

```cpp
// KPSS test (opposite null hypothesis to ADF)
auto kpss_result = kpss_test(prices);

if (kpss_result) {
    auto& result = kpss_result.value();
    std::cout << "KPSS Statistic: " << result.test_statistic << std::endl;
    std::cout << "Is Stationary: " << result.is_stationary_5 << std::endl;
}
```

**Interpretation:**
- **Null Hypothesis**: Series is stationary
- **Rejection**: Test statistic > Critical value → Series is non-stationary

#### Combined Stationarity Check

```cpp
// Uses both ADF and KPSS for robustness
auto is_stationary_result = is_stationary(prices, 0.05);

if (is_stationary_result.value()) {
    std::cout << "Series is stationary!" << std::endl;
}
```

---

### 3. Cointegration Tests (`cointegration.hpp`)

#### Engle-Granger Test (Pairwise)

```cpp
std::vector<double> stock_a_prices = {/* ... */};
std::vector<double> stock_b_prices = {/* ... */};

auto eg_result = engle_granger_test(stock_a_prices, stock_b_prices);

if (eg_result) {
    auto& result = eg_result.value();

    std::cout << "Hedge Ratio (beta): " << result.cointegration_coefficient << std::endl;
    std::cout << "Intercept: " << result.intercept << std::endl;
    std::cout << "Cointegrated: " << result.is_cointegrated_5 << std::endl;

    // Residuals (spread) for pairs trading
    std::vector<double> spread = result.residuals;
}

// Simple check
auto cointegrated = is_cointegrated(stock_a_prices, stock_b_prices, 0.05);
```

**Use Case: Pairs Trading**
```cpp
if (eg_result.value().is_cointegrated_5) {
    double beta = eg_result.value().cointegration_coefficient;
    double alpha = eg_result.value().intercept;

    // Calculate spread
    double spread = stock_a_prices.back() - beta * stock_b_prices.back() - alpha;

    // Trade on mean reversion of spread
    if (spread > 2.0) {
        // Short stock_a, long beta * stock_b
    }
}
```

#### Johansen Test (Multivariate)

```cpp
// Multiple time series
std::vector<std::vector<double>> series = {
    stock_a_prices,
    stock_b_prices,
    stock_c_prices
};

auto johansen_result = johansen_test(series);

if (johansen_result) {
    auto& result = johansen_result.value();

    std::cout << "Cointegration Rank (trace): " << result.rank_trace << std::endl;
    std::cout << "Cointegration Rank (max eigen): " << result.rank_max_eigen << std::endl;

    // Cointegrating vectors
    std::cout << "Eigenvectors:\n" << result.eigenvectors << std::endl;
}
```

---

### 4. GARCH Modeling (`garch.hpp`)

#### Basic Usage

```cpp
// Calculate returns
auto returns = Normalization::calculate_returns(prices, true);

// Fit GARCH(1,1)
GARCH garch;
auto fit_result = garch.fit(returns.value());

if (fit_result) {
    auto& result = fit_result.value();

    // Model parameters
    std::cout << "ω (omega): " << result.omega << std::endl;
    std::cout << "α (alpha): " << result.alpha << std::endl;
    std::cout << "β (beta): " << result.beta << std::endl;

    // Current volatility
    std::cout << "Current Vol: " << garch.get_current_volatility() << std::endl;

    // Persistence
    double persistence = result.alpha + result.beta;
    std::cout << "Persistence: " << persistence << std::endl;
}
```

#### Volatility Forecasting

```cpp
// Forecast 10 days ahead
auto forecast = garch.forecast(10);

if (forecast) {
    for (int i = 0; i < 10; ++i) {
        std::cout << "Day " << (i+1) << " volatility: "
                  << forecast.value().volatility_forecast[i] << std::endl;
    }
}
```

#### Online Updating

```cpp
// In your trading loop
for (const auto& new_bar : live_data) {
    double new_return = std::log(new_bar.close / prev_close);

    auto volatility = garch.update(new_return);
    if (volatility) {
        // Use updated volatility for position sizing
        double position_size = target_risk / volatility.value();
    }

    prev_close = new_bar.close;
}
```

#### Simple Volatility Estimate

```cpp
// Quick one-liner
auto current_vol = estimate_garch_volatility(returns);
```

---

### 5. Kalman Filter (`kalman_filter.hpp`)

#### Basic Kalman Filter

```cpp
// Create filter: 2 states, 1 observation
KalmanFilter kf(2, 1);

// System matrices
// State: [position, velocity]
// Observation: position only
Eigen::MatrixXd F(2, 2);  // State transition
F << 1, 1,
     0, 1;

Eigen::MatrixXd H(1, 2);  // Observation matrix
H << 1, 0;

Eigen::MatrixXd Q = 0.01 * Eigen::MatrixXd::Identity(2, 2);  // Process noise
Eigen::MatrixXd R(1, 1);  // Observation noise
R << 0.1;

Eigen::VectorXd x0(2);  // Initial state
x0 << 0, 0;

Eigen::MatrixXd P0 = Eigen::MatrixXd::Identity(2, 2);  // Initial covariance

// Initialize
kf.initialize(F, H, Q, R, x0, P0);

// Filter observations
for (const auto& price : prices) {
    Eigen::VectorXd obs(1);
    obs << price;

    auto state = kf.filter(obs);
    if (state) {
        double filtered_price = state.value().state(0);
        double velocity = state.value().state(1);

        std::cout << "Filtered price: " << filtered_price << std::endl;
        std::cout << "Velocity: " << velocity << std::endl;
    }
}
```

#### Pre-configured Filters

**Pairs Trading Filter**
```cpp
auto kf_result = create_pairs_trading_filter(0.01, 0.1);

if (kf_result) {
    KalmanFilter& kf = kf_result.value();

    for (double spread_value : spread_series) {
        Eigen::VectorXd obs(1);
        obs << spread_value;

        auto state = kf.filter(obs);
        double filtered_spread = state.value().state(0);
        double spread_momentum = state.value().state(1);

        // Trade on filtered spread
    }
}
```

**Adaptive Beta Filter**
```cpp
auto beta_filter = create_adaptive_beta_filter(0.001, 0.1);

// Track time-varying beta coefficient
for (size_t i = 0; i < y.size(); ++i) {
    Eigen::VectorXd obs(1);
    obs << y[i] / x[i];  // Observation is y/x

    auto state = beta_filter.value().filter(obs);
    double adaptive_beta = state.value().state(0);
}
```

#### Batch Processing with Smoothing

```cpp
// Prepare observation matrix
Eigen::MatrixXd observations(n_obs, 1);
for (int i = 0; i < n_obs; ++i) {
    observations(i, 0) = prices[i];
}

// Forward pass (filtering)
auto filtered_states = kf.filter_batch(observations);

// Backward pass (smoothing)
auto smoothed_states = kf.smooth();

if (smoothed_states) {
    for (const auto& state : smoothed_states.value()) {
        std::cout << "Smoothed price: " << state.state(0) << std::endl;
    }
}
```

---

### 6. Hidden Markov Models (`hmm.hpp`)

#### Basic HMM

```cpp
// Create 3-state HMM with 1D observations
HMM hmm(3, 1);

// Prepare data (returns)
Eigen::MatrixXd observations(n_obs, 1);
for (int i = 0; i < n_obs; ++i) {
    observations(i, 0) = returns[i];
}

// Fit model using Baum-Welch (EM) algorithm
auto fit_result = hmm.fit(observations);

if (fit_result) {
    auto& result = fit_result.value();

    std::cout << "Converged: " << result.converged << std::endl;
    std::cout << "Log-Likelihood: " << result.log_likelihood << std::endl;

    // Transition matrix
    std::cout << "Transition Matrix:\n" << result.transition_matrix << std::endl;

    // Most likely state sequence (Viterbi)
    std::vector<int> states = result.state_sequence;
    std::cout << "Current state: " << states.back() << std::endl;

    // State probabilities (forward-backward)
    Eigen::MatrixXd probs = result.state_probabilities;
}
```

#### Market Regime Detection

```cpp
// Automatically detect market regimes
auto regimes = detect_market_regimes(prices, 20);

if (regimes) {
    auto& result = regimes.value();

    // Current regime
    int current_state = result.state_sequence.back();

    // Classify regime
    HMM temp_hmm(3, 2);
    // ... (need fitted HMM for classification)

    // Regime characteristics
    for (int s = 0; s < 3; ++s) {
        auto [mean, cov] = std::make_pair(
            result.emission_means[s],
            result.emission_covariances[s]
        );

        std::cout << "State " << s << ":" << std::endl;
        std::cout << "  Mean return: " << mean(0) << std::endl;
        std::cout << "  Mean volatility: " << mean(1) << std::endl;
    }
}
```

#### Prediction on New Data

```cpp
// Predict state for new observations
Eigen::MatrixXd new_obs(10, 1);
// ... fill with data

auto predicted_states = hmm.predict(new_obs);
auto state_probabilities = hmm.predict_proba(new_obs);

// Real-time prediction
Eigen::VectorXd single_obs(1);
single_obs << current_return;

auto current_state = hmm.predict_state(single_obs);
```

---

## Usage Examples

### Complete Example: Pairs Trading Strategy

```cpp
#include "trade_ngin/analysis/cointegration.hpp"
#include "trade_ngin/analysis/kalman_filter.hpp"
#include "trade_ngin/analysis/stationarity_tests.hpp"

class PairsTradingStrategy {
public:
    void on_data(const std::vector<double>& stock_a,
                 const std::vector<double>& stock_b) {

        // 1. Test for cointegration
        if (stock_a.size() >= 100 && !cointegration_tested_) {
            auto eg_result = engle_granger_test(stock_a, stock_b);

            if (eg_result && eg_result.value().is_cointegrated_5) {
                hedge_ratio_ = eg_result.value().cointegration_coefficient;
                intercept_ = eg_result.value().intercept;
                cointegrated_ = true;

                // Initialize Kalman Filter for spread tracking
                auto kf = create_pairs_trading_filter(0.01, 0.1);
                if (kf) {
                    kalman_filter_ = kf.value();
                }
            }
            cointegration_tested_ = true;
        }

        if (!cointegrated_) return;

        // 2. Calculate spread
        double spread = stock_a.back() - hedge_ratio_ * stock_b.back() - intercept_;

        // 3. Filter spread with Kalman Filter
        Eigen::VectorXd obs(1);
        obs << spread;

        auto state = kalman_filter_.filter(obs);
        if (!state) return;

        double filtered_spread = state.value().state(0);
        double spread_velocity = state.value().state(1);

        // 4. Generate trading signals
        if (filtered_spread > 2.0 && spread_velocity < 0) {
            // Mean reversion signal: short spread
            // Short stock_a, long hedge_ratio_ * stock_b
            std::cout << "Signal: SHORT spread" << std::endl;
        } else if (filtered_spread < -2.0 && spread_velocity > 0) {
            // Mean reversion signal: long spread
            // Long stock_a, short hedge_ratio_ * stock_b
            std::cout << "Signal: LONG spread" << std::endl;
        }
    }

private:
    bool cointegration_tested_ = false;
    bool cointegrated_ = false;
    double hedge_ratio_ = 0.0;
    double intercept_ = 0.0;
    KalmanFilter kalman_filter_{2, 1};
};
```

### Complete Example: Regime-Adaptive Strategy

```cpp
#include "trade_ngin/analysis/hmm.hpp"
#include "trade_ngin/analysis/garch.hpp"

class RegimeAdaptiveStrategy {
public:
    void on_data(const std::vector<double>& prices) {
        // Update regime detection periodically
        if (prices.size() % 50 == 0 && prices.size() >= 100) {
            update_regime(prices);
        }

        // Update GARCH volatility
        auto returns = Normalization::calculate_returns(prices, true);
        if (returns && returns.value().size() >= 50) {
            if (!garch_.is_fitted()) {
                garch_.fit(returns.value());
            } else {
                double new_return = returns.value().back();
                auto vol = garch_.update(new_return);
                current_volatility_ = vol.value();
            }
        }

        // Adapt strategy based on regime
        if (current_regime_ == 0) {
            // Low volatility, mean-reverting
            use_mean_reversion_strategy(prices);
        } else if (current_regime_ == 1) {
            // Medium volatility, trending
            use_trend_following_strategy(prices);
        } else {
            // High volatility, defensive
            reduce_positions();
        }
    }

private:
    void update_regime(const std::vector<double>& prices) {
        auto regimes = detect_market_regimes(prices, 20);
        if (regimes) {
            current_regime_ = regimes.value().state_sequence.back();
            std::cout << "Current regime: " << current_regime_ << std::endl;
        }
    }

    void use_mean_reversion_strategy(const std::vector<double>& prices) {
        // Implement mean reversion logic
    }

    void use_trend_following_strategy(const std::vector<double>& prices) {
        // Implement trend following logic
    }

    void reduce_positions() {
        // Risk management in high volatility
    }

    int current_regime_ = 0;
    double current_volatility_ = 0.0;
    GARCH garch_;
};
```

---

## Best Practices

### 1. Data Preparation
- Always check for sufficient data before running tests
  - ADF/KPSS: Minimum 20 observations, prefer 50+
  - Cointegration: Minimum 30 observations, prefer 100+
  - GARCH: Minimum 50 observations, prefer 100+
  - HMM: Minimum 50 observations, prefer 200+

### 2. Stationarity Testing
- Use both ADF and KPSS tests together for robustness
- Test both prices and returns
- Returns are typically stationary, prices often are not

### 3. Cointegration
- Only test cointegration between non-stationary series
- Re-test periodically as relationships can break down
- Monitor spread stationarity over time

### 4. Volatility Modeling
- Use log returns for GARCH models
- Check for persistence (α + β < 1)
- High persistence (α + β ≈ 1) indicates long memory

### 5. Kalman Filtering
- Tune process and observation noise parameters carefully
- Lower process noise = slower adaptation
- Higher observation noise = more smoothing

### 6. Regime Detection
- Validate HMM convergence before using results
- Interpret regimes based on emission parameters
- Combine with other indicators for confirmation

---

## Error Handling

All functions return `Result<T>` types for robust error handling:

```cpp
auto result = augmented_dickey_fuller_test(prices);

if (result) {
    // Success - use result.value()
    auto& adf = result.value();
    std::cout << "Test statistic: " << adf.test_statistic << std::endl;
} else {
    // Error - check result.error()
    std::cerr << "Error: " << result.error().message << std::endl;
}
```

Common error codes:
- `INVALID_ARGUMENT`: Invalid input parameters
- `INVALID_STATE`: Operation called on uninitialized object
- `CALCULATION_ERROR`: Numerical computation failed

---

## Performance Considerations

- **PCA**: O(n * m²) for n samples, m features
- **ADF/KPSS**: O(n²) where n is series length
- **Cointegration**: O(n²) for pairwise, O(n * m³) for Johansen
- **GARCH**: O(n * iterations) for fitting
- **Kalman Filter**: O(n * s²) for n observations, s states
- **HMM**: O(n * k² * iterations) for n observations, k states

For large datasets:
- Use batch processing where available
- Consider downsampling for preliminary analysis
- Cache results of expensive operations

---

## References

### Statistical Tests
- **ADF Test**: Dickey, D. A., & Fuller, W. A. (1979)
- **KPSS Test**: Kwiatkowski, D., et al. (1992)
- **Engle-Granger**: Engle, R. F., & Granger, C. W. J. (1987)
- **Johansen**: Johansen, S. (1991)

### Models
- **GARCH**: Bollerslev, T. (1986)
- **Kalman Filter**: Kalman, R. E. (1960)
- **HMM**: Baum, L. E., & Petrie, T. (1966)

---

## License

This module is part of the trade-ngin project and inherits its license.

## Support

For issues, questions, or contributions, please refer to the main trade-ngin repository.
