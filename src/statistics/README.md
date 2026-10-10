# Statistics Module

## Overview

The statistics module is a library of quantitative analysis classes: data transformers,
pre-processing, stationarity and cointegration tests, regressions, volatility models and state
estimators. Every class is in the `trade_ngin::statistics` namespace; the helper functions are in
`trade_ngin::statistics::utils`.

It is a standalone library. No strategy, portfolio, risk, backtest or live source includes these
headers, so nothing here takes part in sizing, the rebalance or the stored results. The statistics
the runners store with a run (Sharpe ratio, volatility, drawdown and the rest) are computed in
`src/backtest/backtest_metrics_calculator.cpp`, `src/live/live_metrics_calculator.cpp` and
`src/live/live_historical_metrics.cpp`, not here.

---

## File layout

```
include/trade_ngin/statistics/
├── statistics.hpp                     # convenience header: includes everything below
├── statistics_tools.hpp               # deprecated redirect to statistics.hpp
├── statistics_common.hpp              # every config struct and result type
├── statistics_utils.hpp               # helper function declarations (namespace utils)
├── critical_values.hpp                # critical value tables
├── validation.hpp                     # input validation
├── hurst_exponent.hpp
├── base/
│   ├── data_transformer.hpp           # DataTransformer
│   ├── statistical_test.hpp           # StatisticalTest
│   ├── volatility_model.hpp           # VolatilityModel
│   └── state_estimator.hpp            # StateEstimator
├── transformers/                      # normalizer.hpp, pca.hpp
├── preprocessing/                     # outlier_handler.hpp, missing_data_handler.hpp
├── tests/                             # adf_test.hpp, kpss_test.hpp, phillips_perron_test.hpp,
│                                      #   variance_ratio_test.hpp, johansen_test.hpp,
│                                      #   engle_granger_test.hpp
├── regression/                        # ols_regression.hpp, ridge_regression.hpp, lasso_regression.hpp
├── volatility/                        # garch.hpp, egarch.hpp, gjr_garch.hpp, dcc_garch.hpp
└── state_estimation/                  # kalman_filter.hpp, extended_kalman_filter.hpp, hmm.hpp,
                                       #   markov_switching.hpp

src/statistics/
├── hurst_exponent.cpp
├── utils/statistics_utils.cpp
├── transformers/, preprocessing/, tests/, regression/, volatility/, state_estimation/
└──   one .cpp per class above
```

---

## Implemented classes

### Transformers and pre-processing

| Class | Header | Purpose |
|---|---|---|
| `Normalizer` | `transformers/normalizer.hpp` | normalisation; a `DataTransformer` |
| `PCA` | `transformers/pca.hpp` | principal component analysis; a `DataTransformer` |
| `OutlierHandler` | `preprocessing/outlier_handler.hpp` | `detect` and `handle` outliers in a series |
| `MissingDataHandler` | `preprocessing/missing_data_handler.hpp` | handles missing values in a series or a matrix |

### Statistical tests

| Class | Header | Purpose |
|---|---|---|
| `ADFTest` | `tests/adf_test.hpp` | augmented Dickey-Fuller unit root test; a `StatisticalTest` |
| `KPSSTest` | `tests/kpss_test.hpp` | KPSS stationarity test; a `StatisticalTest` |
| `PhillipsPerronTest` | `tests/phillips_perron_test.hpp` | Phillips-Perron unit root test; a `StatisticalTest` |
| `VarianceRatioTest` | `tests/variance_ratio_test.hpp` | variance ratio test; a `StatisticalTest` |
| `JohansenTest` | `tests/johansen_test.hpp` | cointegration of several series; returns a `CointegrationResult` |
| `EngleGrangerTest` | `tests/engle_granger_test.hpp` | two-step cointegration of a pair |
| `HurstExponent` | `hurst_exponent.hpp` | Hurst exponent; returns a `HurstResult` |

### Regression

| Class | Header | Purpose |
|---|---|---|
| `OLSRegression` | `regression/ols_regression.hpp` | ordinary least squares |
| `RidgeRegression` | `regression/ridge_regression.hpp` | ridge (L2) regression |
| `LassoRegression` | `regression/lasso_regression.hpp` | lasso (L1) regression |

### Volatility models

| Class | Header | Purpose |
|---|---|---|
| `GARCH` | `volatility/garch.hpp` | GARCH; a `VolatilityModel` |
| `EGARCH` | `volatility/egarch.hpp` | exponential GARCH; a `VolatilityModel` |
| `GJRGARCH` | `volatility/gjr_garch.hpp` | GJR-GARCH; a `VolatilityModel` |
| `DCCGARCH` | `volatility/dcc_garch.hpp` | dynamic conditional correlation GARCH over several series |

### State estimation

| Class | Header | Purpose |
|---|---|---|
| `KalmanFilter` | `state_estimation/kalman_filter.hpp` | linear Kalman filter; a `StateEstimator` |
| `ExtendedKalmanFilter` | `state_estimation/extended_kalman_filter.hpp` | extended Kalman filter; a `StateEstimator` |
| `HMM` | `state_estimation/hmm.hpp` | hidden Markov model; a `StateEstimator` |
| `MarkovSwitching` | `state_estimation/markov_switching.hpp` | Markov switching model; a `StateEstimator` |

Each class takes its config struct from `statistics_common.hpp` (for example `ADFTestConfig`,
`GARCHConfig`, `KalmanFilterConfig`). Read the struct there for the fields and defaults.

---

## The four base interfaces

| Interface | Methods |
|---|---|
| `DataTransformer` | `fit(matrix)`, `transform(matrix)`, `fit_transform(matrix)`, `inverse_transform(matrix)`, `is_fitted()` |
| `StatisticalTest` | `test(const std::vector<double>&)` returning `Result<TestResult>`, `get_name()` |
| `VolatilityModel` | `fit(returns)`, `forecast(n_periods)`, `get_current_volatility()`, `update(new_return)`, `is_fitted()` |
| `StateEstimator` | `initialize(initial_state)`, `predict()`, `update(observation)`, `get_state()`, `is_initialized()` |

Every method except `is_fitted()`, `is_initialized()` and `get_name()` returns `Result<T>`; check it before reading the value.

```cpp
#include "trade_ngin/statistics/statistics.hpp"

using namespace trade_ngin::statistics;

ADFTestConfig config;                       // regression CONSTANT, max_lags -1 (automatic)
ADFTest adf(config);

std::vector<double> series = /* ... */;
auto result = adf.test(series);
if (result.is_error()) {
    // result.error()->what()
} else {
    const TestResult& t = result.value();   // statistic, p_value, critical_value, reject_null
}
```

---

## Helper functions

`statistics_utils.hpp` declares, in `trade_ngin::statistics::utils`: `calculate_mean`,
`calculate_variance`, `calculate_std`, `calculate_median`, `calculate_iqr`, `autocorrelation` and
`difference`.

---

## Testing

Every test is built into one binary, `trade_ngin_tests`, and `ctest` lists each case by its suite
name. The statistics tests are in `tests/statistics/`, one file per class, plus
`test_statistics_integration.cpp`, `test_convergence_monitoring.cpp` and `test_critical_values.cpp`:

```bash
cd build
ctest -R "GARCH|Kalman|HMM|ADF|KPSS|Johansen|Regression" --output-on-failure
```

---

## Dependencies

- Eigen3 for linear algebra
- `trade_ngin` core for `Result<T>` (`include/trade_ngin/core/error.hpp`)
