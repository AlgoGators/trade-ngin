#include "system/trend_strategy.hpp"
#include "data/dataframe.hpp"
#include <map>
#include <unordered_map>
#include <memory>
#include <iostream>

const double initial_capital = 500000.0;

// Strategy parameters
std::unordered_map<std::string, double> ma_params = {
    {"short_window_1", 10},
    {"short_window_2", 20},
    {"short_window_3", 30},
    {"short_window_4", 40},
    {"short_window_5", 50},
    {"short_window_6", 60},
    {"long_window_1", 100},
    {"long_window_2", 150},
    {"long_window_3", 200}
};

std::unordered_map<std::string, double> vol_params = {
    {"window", 20},
    {"target_vol", 0.20},  // Increased from 0.15
    {"high_vol_threshold", 1.5},  // More tolerant threshold
    {"low_vol_threshold", 0.5}   // More tolerant threshold
};

std::unordered_map<std::string, double> regime_params = {
    {"lookback", 60},
    {"threshold", 0.02}  // More sensitive to regime changes
};

std::unordered_map<std::string, double> momentum_params = {
    {"lookback", 60},  // Extended from 40
    {"threshold", 0.02}  // More sensitive momentum signal
};

std::unordered_map<std::string, double> weight_params = {
    {"short_weight", 0.6},  // Reduced from 0.7
    {"long_weight", 0.4},   // Increased from 0.3
    {"base_size", 0.01}     // Increased from 0.005 (1% of capital)
};

// ... rest of the file ... 