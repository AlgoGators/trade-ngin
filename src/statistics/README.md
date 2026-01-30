# Statistics Module

## Overview

The statistics module provides quantitative analysis capabilities for the trading engine, including data transformers, stationarity tests, cointegration analysis, volatility modeling, and state estimation.

> **Note**: This module is documented in detail in [statistics_module_deliverable.md](../docs/statistics_module_deliverable.md).

---

## Architecture

```
statistics/
└── statistics_tools.cpp    # All implementations

include/trade_ngin/statistics/
└── statistics_tools.hpp    # All declarations
```

All classes are in the `trade_ngin::statistics` namespace.

---

## Available Models

### Data Transformers

| Model | Class | Purpose |
|-------|-------|---------|
| **Normalizer** | `Normalizer` | Z-Score, Min-Max, Robust normalization |
| **PCA** | `PCA` | Principal Component Analysis with variance threshold |

### Statistical Tests

| Model | Class | Purpose |
|-------|-------|---------|
| **ADF Test** | `ADFTest` | Augmented Dickey-Fuller stationarity test |
| **KPSS Test** | `KPSSTest` | Kwiatkowski-Phillips-Schmidt-Shin test |
| **Johansen Test** | `JohansenTest` | Multi-series cointegration test |
| **Engle-Granger** | `EngleGrangerTest` | Two-step cointegration test |

### Volatility Models

| Model | Class | Purpose |
|-------|-------|---------|
| **GARCH(1,1)** | `GARCH` | Generalized Autoregressive Conditional Heteroskedasticity |

### State Estimation

| Model | Class | Purpose |
|-------|-------|---------|
| **Kalman Filter** | `KalmanFilter` | Linear state estimation |
| **HMM** | `HMM` | Hidden Markov Model for regime detection |

---

## Usage Examples

### Normalizer

```cpp
#include "trade_ngin/statistics/statistics_tools.hpp"

using namespace trade_ngin::statistics;

// Configure normalization
NormalizationConfig config;
config.method = NormalizationMethod::Z_SCORE;  // or MIN_MAX, ROBUST

Normalizer normalizer(config);

// Fit to data
Eigen::MatrixXd data(100, 5);  // 100 samples, 5 features
auto fit_result = normalizer.fit(data);
if (fit_result.is_error()) {
    // Handle error
}

// Transform new data
auto transformed = normalizer.transform(new_data);
if (transformed.is_ok()) {
    Eigen::MatrixXd normalized = transformed.value();
}

// Inverse transform back to original scale
auto original = normalizer.inverse_transform(normalized);
```

### PCA

```cpp
// Configure PCA
PCAConfig pca_config;
pca_config.n_components = 0;           // Auto-select based on variance
pca_config.variance_threshold = 0.95;  // Keep 95% of variance
pca_config.whiten = false;

PCA pca(pca_config);

// Fit and transform
auto fit_result = pca.fit(data);
auto transformed = pca.transform(data);

// Access components
Eigen::MatrixXd components = pca.get_components();
Eigen::VectorXd explained_variance = pca.get_explained_variance_ratio();
```

### ADF Test (Stationarity)

```cpp
// Configure test
ADFTestConfig adf_config;
adf_config.regression = ADFRegression::CONSTANT;  // Include constant term
adf_config.max_lags = 0;  // Auto-select via AIC

ADFTest adf(adf_config);

// Run test
std::vector<double> prices = {...};  // Time series
auto result = adf.test(prices);

if (result.is_ok()) {
    TestResult test = result.value();
    std::cout << "ADF Statistic: " << test.statistic << std::endl;
    std::cout << "Reject H0 (stationary): " << (test.reject_null ? "Yes" : "No") << std::endl;
    
    // test.reject_null == true means series is stationary
}
```

### KPSS Test

```cpp
KPSSTestConfig kpss_config;
kpss_config.regression = KPSSRegression::LEVEL;  // Level stationarity

KPSSTest kpss(kpss_config);
auto result = kpss.test(prices);

// Note: KPSS null hypothesis is opposite of ADF
// test.reject_null == true means series is NOT stationary
```

### Johansen Cointegration Test

```cpp
JohansenTestConfig johansen_config;
johansen_config.max_lags = 2;
johansen_config.significance_level = 0.05;

JohansenTest johansen(johansen_config);

// Test for cointegration between multiple series
std::vector<std::vector<double>> series = {
    prices_ES,  // E-mini S&P
    prices_NQ,  // Nasdaq
    prices_YM   // Dow
};

auto result = johansen.test(series);
if (result.is_ok()) {
    JohansenResult jr = result.value();
    std::cout << "Cointegration rank: " << jr.rank << std::endl;
    // rank = 0: no cointegration
    // rank = 1: one cointegrating relationship
    // etc.
}
```

### Engle-Granger Cointegration Test

```cpp
EngleGrangerTestConfig eg_config;
eg_config.significance_level = 0.05;

EngleGrangerTest eg(eg_config);

// Two-step approach for pair cointegration
std::vector<double> series_y = {...};  // Dependent
std::vector<double> series_x = {...};  // Independent

auto result = eg.test(series_y, series_x);
if (result.is_ok()) {
    EngleGrangerResult egr = result.value();
    std::cout << "Cointegrated: " << (egr.cointegrated ? "Yes" : "No") << std::endl;
    std::cout << "Hedge ratio: " << egr.hedge_ratio << std::endl;
}
```

### GARCH(1,1) Volatility

```cpp
GARCHConfig garch_config;
garch_config.p = 1;  // ARCH order
garch_config.q = 1;  // GARCH order

GARCH garch(garch_config);

// Fit to return series
std::vector<double> returns = {...};
auto fit_result = garch.fit(returns);

if (fit_result.is_ok()) {
    // Get estimated parameters
    double omega = garch.get_omega();
    double alpha = garch.get_alpha();
    double beta = garch.get_beta();
    
    // Get current conditional volatility
    double current_vol = garch.get_conditional_volatility();
    
    // Forecast future volatility
    std::vector<double> forecasts = garch.forecast(5);  // 5 steps ahead
}

// Update with new data
garch.update(new_return);
double updated_vol = garch.get_conditional_volatility();
```

### Kalman Filter

```cpp
KalmanFilterConfig kf_config;
kf_config.state_dim = 2;   // Number of state variables
kf_config.obs_dim = 1;     // Number of observations

KalmanFilter kf(kf_config);

// Set state transition matrix (A)
Eigen::MatrixXd A(2, 2);
A << 1, 1,
     0, 1;
kf.set_transition_matrix(A);

// Set observation matrix (H)
Eigen::MatrixXd H(1, 2);
H << 1, 0;
kf.set_observation_matrix(H);

// Set noise covariances
Eigen::MatrixXd Q(2, 2);  // Process noise
Q << 0.1, 0,
     0, 0.1;
kf.set_process_noise(Q);

Eigen::MatrixXd R(1, 1);  // Measurement noise
R << 1.0;
kf.set_measurement_noise(R);

// Initialize state
Eigen::VectorXd x0(2);
x0 << 0, 0;
Eigen::MatrixXd P0 = Eigen::MatrixXd::Identity(2, 2);
kf.initialize(x0, P0);

// Predict-update cycle
for (const auto& observation : observations) {
    kf.predict();
    
    Eigen::VectorXd z(1);
    z << observation;
    kf.update(z);
    
    Eigen::VectorXd state = kf.get_state();
    Eigen::MatrixXd covariance = kf.get_covariance();
}
```

### Hidden Markov Model

```cpp
HMMConfig hmm_config;
hmm_config.n_states = 2;      // e.g., Bull/Bear regimes
hmm_config.obs_dim = 1;       // 1D observations (returns)
hmm_config.max_iterations = 100;
hmm_config.convergence_threshold = 1e-4;

HMM hmm(hmm_config);

// Prepare observations as matrix (T x obs_dim)
Eigen::MatrixXd observations(returns.size(), 1);
for (size_t i = 0; i < returns.size(); ++i) {
    observations(i, 0) = returns[i];
}

// Fit model using Baum-Welch (EM algorithm)
auto fit_result = hmm.fit(observations);

if (fit_result.is_ok()) {
    // Get state means and covariances
    std::vector<Eigen::VectorXd> means = hmm.get_means();
    std::vector<Eigen::MatrixXd> covs = hmm.get_covariances();
    
    // Get transition matrix
    Eigen::MatrixXd transitions = hmm.get_transition_matrix();
    
    // Decode most likely state sequence (Viterbi)
    std::vector<int> states = hmm.decode(observations);
    
    // Get current regime probability
    Eigen::VectorXd regime_probs = hmm.predict_proba(observations);
}
```

---

## Configuration Structures

### NormalizationConfig

```cpp
struct NormalizationConfig {
    NormalizationMethod method = NormalizationMethod::Z_SCORE;
    // Z_SCORE: (x - mean) / std
    // MIN_MAX: (x - min) / (max - min)
    // ROBUST: (x - median) / IQR
};
```

### PCAConfig

```cpp
struct PCAConfig {
    int n_components = 0;             // 0 = auto-select
    double variance_threshold = 0.95; // For auto-selection
    bool whiten = false;              // Decorrelate and scale
};
```

### ADFTestConfig

```cpp
struct ADFTestConfig {
    ADFRegression regression = ADFRegression::CONSTANT;
    // NONE: No constant or trend
    // CONSTANT: Include constant
    // CONSTANT_TREND: Include constant and linear trend
    
    int max_lags = 0;  // 0 = auto-select via AIC
};
```

### GARCHConfig

```cpp
struct GARCHConfig {
    int p = 1;                  // ARCH order
    int q = 1;                  // GARCH order
    int max_iterations = 1000;  // Max optimization iterations
    double convergence_threshold = 1e-6;
};
```

### KalmanFilterConfig

```cpp
struct KalmanFilterConfig {
    int state_dim = 1;   // Dimension of state vector
    int obs_dim = 1;     // Dimension of observation vector
};
```

### HMMConfig

```cpp
struct HMMConfig {
    int n_states = 2;                   // Number of hidden states
    int obs_dim = 1;                    // Observation dimension
    int max_iterations = 100;           // Baum-Welch iterations
    double convergence_threshold = 1e-4;
};
```

---

## Error Handling

All methods return `Result<T>` for error handling:

```cpp
auto result = model.fit(data);

if (result.is_error()) {
    std::cerr << "Error: " << result.error()->what() << std::endl;
    std::cerr << "Code: " << static_cast<int>(result.error()->code()) << std::endl;
    return;
}

// Use result.value()
```

Common error scenarios:
- Empty or insufficient data
- NaN/Inf values in data
- Non-convergence (GARCH, HMM)
- Dimension mismatches (Kalman Filter)

---

## Helper Functions

The module includes utility functions (in anonymous namespace):

```cpp
// Basic statistics
double calculate_mean(const std::vector<double>& data);
double calculate_variance(const std::vector<double>& data, double mean);
double calculate_std(const std::vector<double>& data, double mean);
double calculate_median(std::vector<double> data);
double calculate_iqr(std::vector<double> data);

// Time series operations
std::vector<double> autocorrelation(const std::vector<double>& data, int max_lag);
std::vector<double> difference(const std::vector<double>& data, int order = 1);
```

---

## Testing

```bash
# Run statistics tests
cd build
ctest -R statistics --output-on-failure
```

Test file: `tests/statistics/test_statistics_tools.cpp`

---

## Known Issues & Roadmap

See [statistics_module_deliverable.md](../docs/statistics_module_deliverable.md) for:
- Numerical stability issues (`.inverse()` usage)
- HMM log-space implementation needs
- Critical value table improvements
- Planned new models (EGARCH, Hurst, Markov Switching)

---

## Dependencies

- **Eigen3**: Linear algebra operations
- **trade_ngin::core**: Error handling (`Result<T>`)

---

## References

- [Strategy Module](../strategy/README.md) - Uses statistics for indicators
- MacKinnon (1996) - ADF critical values
- Kwiatkowski et al. (1992) - KPSS critical values  
- Hamilton (1989) - Markov Switching
- Bollerslev (1986) - GARCH
- Rabiner (1989) - HMM tutorial
