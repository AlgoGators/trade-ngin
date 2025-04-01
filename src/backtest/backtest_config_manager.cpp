#include "trade_ngin/backtest/backtest_config_manager.hpp"
#include "trade_ngin/core/logger.hpp"
#include <fstream>
#include <chrono>
#include <iostream>

namespace trade_ngin {
namespace backtest {

BacktestConfigManager::BacktestConfigManager(
    const std::filesystem::path& config_dir)
    : config_dir_(config_dir) {
    
    // Create config directory if it doesn't exist
    if (!std::filesystem::exists(config_dir_)) {
        std::filesystem::create_directories(config_dir_);
    }
}

Result<BacktestConfigManager> BacktestConfigManager::create_default() {
    BacktestConfigManager config;
    
    // Set default backtest config
    BacktestConfig backtest_config;
    backtest_config.store_trade_details = true;
    backtest_config.csv_output_path = "apps/backtest/results";
    
    // Set default portfolio config
    auto& portfolio_config = backtest_config.portfolio_config;
    portfolio_config.initial_capital = 1000000.0;  // $1M
    portfolio_config.use_risk_management = true;
    portfolio_config.use_optimization = true;
    
    // Set default risk config
    auto& risk_config = portfolio_config.risk_config;
    risk_config.capital = portfolio_config.initial_capital;
    risk_config.confidence_level = 0.99;
    risk_config.lookback_period = 252;
    risk_config.var_limit = 0.15;
    risk_config.jump_risk_limit = 0.10;
    risk_config.max_correlation = 0.7;
    risk_config.max_gross_leverage = 4.0;
    risk_config.max_net_leverage = 2.0;
    
    // Set default optimization config
    auto& opt_config = portfolio_config.opt_config;
    opt_config.tau = 1.0;
    opt_config.capital = portfolio_config.initial_capital;
    opt_config.asymmetric_risk_buffer = 0.1;
    opt_config.cost_penalty_scalar = 10;
    opt_config.max_iterations = 100;
    opt_config.convergence_threshold = 1e-6;
    
    // Set default strategy backtest config
    auto& strategy_backtest_config = backtest_config.strategy_config;
    strategy_backtest_config.asset_class = AssetClass::FUTURES;
    strategy_backtest_config.data_freq = DataFrequency::DAILY;
    
    // Set default date range (2 years until now)
    auto now = std::chrono::system_clock::now();
    strategy_backtest_config.end_date = now;
    
    auto start_time = now - std::chrono::hours(24 * 365 * 2);  // 2 years ago
    strategy_backtest_config.start_date = start_time;
    
    strategy_backtest_config.commission_rate = 0.0005;  // 5 basis points
    strategy_backtest_config.slippage_model = 1.0;      // 1 bp 
    strategy_backtest_config.initial_capital = portfolio_config.initial_capital;
    
    // Add some default futures symbols
    strategy_backtest_config.symbols = {
        "ES.v.0", "NQ.v.0", "YM.v.0", "RTY.v.0",  // Equity index futures
        "ZB.v.0", "ZN.v.0", "ZF.v.0", "ZT.v.0",   // US Treasury futures
        "GC.v.0", "SI.v.0", "HG.v.0", "PL.v.0",   // Metals
        "CL.v.0", "NG.v.0", "HO.v.0", "RB.v.0",   // Energy
        "ZC.v.0", "ZW.v.0", "ZS.v.0", "ZM.v.0",   // Grains
        "6E.v.0", "6J.v.0", "6B.v.0", "6A.v.0"    // Currencies
    };
    
    // Set default strategy config 
    StrategyConfig strategy_config;
    strategy_config.capital_allocation = portfolio_config.initial_capital;
    strategy_config.asset_classes = {AssetClass::FUTURES};
    strategy_config.frequencies = {DataFrequency::DAILY};
    strategy_config.max_drawdown = 0.4;  // 40% max drawdown
    strategy_config.max_leverage = 4.0;
    strategy_config.save_positions = false;
    strategy_config.save_signals = false;
    strategy_config.save_executions = false;
    
    // Add position limits and costs for each symbol
    for (const auto& symbol : strategy_backtest_config.symbols) {
        strategy_config.position_limits[symbol] = 1000.0;  // Max 1000 units per symbol
        strategy_config.costs[symbol] = strategy_backtest_config.commission_rate;
    }
    
    // Set default trend following config
    TrendFollowingConfig trend_config;
    trend_config.weight = 1.0 / strategy_backtest_config.symbols.size();  // Equal weight
    trend_config.risk_target = 0.2;       // Target 20% annualized risk
    trend_config.idm = 2.5;               // Instrument diversification multiplier
    trend_config.use_position_buffering = true;
    trend_config.ema_windows = {
        {2, 8}, {4, 16}, {8, 32}, {16, 64}, {32, 128}, {64, 256}
    };
    trend_config.vol_lookback_short = 32;  // Short vol lookback
    trend_config.vol_lookback_long = 252;  // Long vol lookback
    trend_config.fdm = {
        {1, 1.0}, {2, 1.03}, {3, 1.08}, {4, 1.13}, {5, 1.19}, {6, 1.26}
    };
    
    // Set the configs
    config.set_backtest_config(backtest_config);
    config.set_strategy_config(strategy_config);
    config.set_trend_config(trend_config);
    
    return Result<BacktestConfigManager>(std::move(config));
}

Result<void> BacktestConfigManager::save(const std::string& filename) const {
    try {
        std::filesystem::path file_path = config_dir_ / filename;
        
        // Create JSON representation
        nlohmann::json config_json = to_json();
        
        // Create parent directories if they don't exist
        if (!std::filesystem::exists(config_dir_)) {
            std::filesystem::create_directories(config_dir_);
        }
        
        // Write to file with pretty formatting
        std::ofstream file(file_path);
        if (!file.is_open()) {
            return make_error<void>(
                ErrorCode::IO_ERROR,
                "Failed to open file for writing: " + file_path.string(),
                "BacktestConfigManager"
            );
        }
        
        file << std::setw(4) << config_json << std::endl;
        
        INFO("Saved backtest configuration to: " + file_path.string());
        return Result<void>();
        
    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error saving backtest config: ") + e.what(),
            "BacktestConfigManager"
        );
    }
}

Result<void> BacktestConfigManager::load(const std::string& filename) {
    try {
        std::filesystem::path file_path = config_dir_ / filename;
        
        // Check if file exists
        if (!std::filesystem::exists(file_path)) {
            return make_error<void>(
                ErrorCode::FILE_NOT_FOUND,
                "Config file not found: " + file_path.string(),
                "BacktestConfigManager"
            );
        }
        
        // Read JSON from file
        std::ifstream file(file_path);
        if (!file.is_open()) {
            return make_error<void>(
                ErrorCode::IO_ERROR,
                "Failed to open file for reading: " + file_path.string(),
                "BacktestConfigManager"
            );
        }
        
        nlohmann::json config_json;
        file >> config_json;
        
        // Parse JSON
        from_json(config_json);
        
        INFO("Loaded backtest configuration from: " + file_path.string());
        return Result<void>();
        
    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error loading backtest config: ") + e.what(),
            "BacktestConfigManager"
        );
    }
}

Result<void> BacktestConfigManager::apply_to_engine(BacktestEngine& engine) const {
    // Currently no method to update engine config directly
    // This would modify the engine's configuration if such methods were available
    
    WARN("BacktestConfigManager::apply_to_engine is not fully implemented.");
    WARN("Create a new engine instance with this configuration instead.");
    
    return Result<void>();
}

nlohmann::json BacktestConfigManager::to_json() const {
    nlohmann::json j;
    j["backtest_config"] = backtest_config_.to_json();
    j["strategy_config"] = strategy_config_.to_json();
    j["trend_config"] = trend_config_.to_json();
    return j;
}

void BacktestConfigManager::from_json(const nlohmann::json& j) {
    if (j.contains("backtest_config")) {
        backtest_config_.from_json(j.at("backtest_config"));
    }
    
    if (j.contains("strategy_config")) {
        strategy_config_.from_json(j.at("strategy_config"));
    }
    
    if (j.contains("trend_config")) {
        trend_config_.from_json(j.at("trend_config"));
    }
}

} // namespace backtest
} // namespace trade_ngin 