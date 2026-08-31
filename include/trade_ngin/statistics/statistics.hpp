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

// Volatility models
#include "trade_ngin/statistics/volatility/garch.hpp"

// State estimation
#include "trade_ngin/statistics/state_estimation/kalman_filter.hpp"
#include "trade_ngin/statistics/state_estimation/hmm.hpp"
