#pragma once

// Common types and configuration
#include "trade_ngin/statistics/statistics_common.hpp"
#include "trade_ngin/statistics/statistics_utils.hpp"
#include "trade_ngin/statistics/critical_values.hpp"
#include "trade_ngin/statistics/validation.hpp"

// Base classes
#include "trade_ngin/statistics/base/data_transformer.hpp"
#include "trade_ngin/statistics/base/statistical_test.hpp"
#include "trade_ngin/statistics/base/volatility_model.hpp"
#include "trade_ngin/statistics/base/state_estimator.hpp"

// Transformers
#include "trade_ngin/statistics/transformers/normalizer.hpp"
#include "trade_ngin/statistics/transformers/pca.hpp"

// Statistical tests
#include "trade_ngin/statistics/tests/adf_test.hpp"
#include "trade_ngin/statistics/tests/kpss_test.hpp"
#include "trade_ngin/statistics/tests/johansen_test.hpp"
#include "trade_ngin/statistics/tests/engle_granger_test.hpp"
#include "trade_ngin/statistics/tests/phillips_perron_test.hpp"
#include "trade_ngin/statistics/tests/variance_ratio_test.hpp"

// Volatility models
#include "trade_ngin/statistics/volatility/garch.hpp"
#include "trade_ngin/statistics/volatility/egarch.hpp"
#include "trade_ngin/statistics/volatility/gjr_garch.hpp"
#include "trade_ngin/statistics/volatility/dcc_garch.hpp"

// State estimation
#include "trade_ngin/statistics/state_estimation/kalman_filter.hpp"
#include "trade_ngin/statistics/state_estimation/hmm.hpp"
#include "trade_ngin/statistics/state_estimation/markov_switching.hpp"
#include "trade_ngin/statistics/state_estimation/extended_kalman_filter.hpp"

// Regression models
#include "trade_ngin/statistics/regression/ols_regression.hpp"
#include "trade_ngin/statistics/regression/ridge_regression.hpp"
#include "trade_ngin/statistics/regression/lasso_regression.hpp"

// Preprocessing
#include "trade_ngin/statistics/preprocessing/outlier_handler.hpp"
#include "trade_ngin/statistics/preprocessing/missing_data_handler.hpp"

// Analysis tools
#include "trade_ngin/statistics/hurst_exponent.hpp"
