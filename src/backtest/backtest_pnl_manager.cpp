// src/backtest/backtest_pnl_manager.cpp
//
// BacktestPnLManager Implementation - Single Source of Truth for Backtest PnL
//
// DEBUG TAGS TO SEARCH FOR IN LOGS:
// ================================
// [BACKTEST_PNL] CALC       - Individual PnL calculations with full formula breakdown
// [BACKTEST_PNL] POINT_VALUE - Point value lookups and fallback decisions
// [BACKTEST_PNL] DAILY_TOTAL - Daily total PnL summaries
// [BACKTEST_PNL] PORTFOLIO   - Portfolio value updates
// [BACKTEST_PNL] POSITION    - Position-level PnL tracking
// [BACKTEST_PNL] PREV_CLOSE  - Previous close price updates
// [BACKTEST_PNL] RESET       - Reset operations
// [BACKTEST_PNL] ERROR       - Any calculation errors
//

#include "trade_ngin/backtest/backtest_pnl_manager.hpp"
#include <cmath>

namespace trade_ngin {
namespace backtest {

// ============================================================================
// MAIN ENTRY POINT: Calculate daily PnL for all positions
// ============================================================================
BacktestPnLManager::DailyPnLResult BacktestPnLManager::calculate_daily_pnl(
    const Timestamp& timestamp,
    const std::unordered_map<std::string, Position>& positions,
    const std::unordered_map<std::string, double>& current_close_prices,
    double commissions) {
    
    DailyPnLResult result;
    result.date_str = format_date(timestamp);
    current_date_str_ = result.date_str;
    result.total_commissions = commissions;
    
    log_info("[BACKTEST_PNL] DAILY_TOTAL: Starting PnL calculation for date=" + result.date_str +
             ", positions=" + std::to_string(positions.size()) +
             ", commissions=" + std::to_string(commissions));
    
    // Reset daily tracking
    reset_daily();
    
    double total_pnl = 0.0;
    
    for (const auto& [symbol, position] : positions) {
        double quantity = static_cast<double>(position.quantity);
        
        // Skip zero quantity positions
        if (std::abs(quantity) < 1e-8) {
            log_debug("[BACKTEST_PNL] CALC: Skipping " + symbol + " - zero quantity");
            continue;
        }
        
        // Get current close price
        auto curr_it = current_close_prices.find(symbol);
        if (curr_it == current_close_prices.end()) {
            log_warn("[BACKTEST_PNL] ERROR: No current close price for " + symbol + 
                     " on " + result.date_str);
            PositionPnLResult pos_result;
            pos_result.symbol = symbol;
            pos_result.valid = false;
            pos_result.error_message = "No current close price";
            result.position_results[symbol] = pos_result;
            continue;
        }
        double current_close = curr_it->second;
        
        // Get previous close price
        if (!has_previous_close(symbol)) {
            log_info("[BACKTEST_PNL] CALC: No previous close for " + symbol + 
                     " (first day) - storing current close=" + std::to_string(current_close) + 
                     " for next day, PnL=0");
            
            // Store for next iteration
            set_previous_close(symbol, current_close);
            
            PositionPnLResult pos_result;
            pos_result.symbol = symbol;
            pos_result.quantity = quantity;
            pos_result.current_close = current_close;
            pos_result.daily_pnl = 0.0;
            pos_result.valid = true;
            result.position_results[symbol] = pos_result;
            continue;
        }
        
        double previous_close = get_previous_close(symbol);
        
        // Calculate PnL for this position
        PositionPnLResult pos_result = calculate_position_pnl(symbol, quantity, previous_close, current_close);
        result.position_results[symbol] = pos_result;
        
        if (pos_result.valid) {
            total_pnl += pos_result.daily_pnl;
            position_daily_pnl_[symbol] = pos_result.daily_pnl;
            position_cumulative_pnl_[symbol] += pos_result.daily_pnl;
            
            log_info("[BACKTEST_PNL] POSITION: " + symbol + 
                     " daily_pnl=" + std::to_string(pos_result.daily_pnl) +
                     ", cumulative_pnl=" + std::to_string(position_cumulative_pnl_[symbol]));
        }
    }
    
    // Calculate net PnL and update portfolio value
    result.total_daily_pnl = total_pnl;
    result.net_daily_pnl = total_pnl - commissions;
    
    // Update internal tracking
    daily_total_pnl_ = total_pnl;
    cumulative_total_pnl_ += result.net_daily_pnl;
    current_portfolio_value_ += result.net_daily_pnl;
    result.new_portfolio_value = current_portfolio_value_;
    
    log_info("[BACKTEST_PNL] DAILY_TOTAL: date=" + result.date_str +
             ", gross_pnl=" + std::to_string(total_pnl) +
             ", commissions=" + std::to_string(commissions) +
             ", net_pnl=" + std::to_string(result.net_daily_pnl) +
             ", portfolio_value=" + std::to_string(current_portfolio_value_));
    
    log_info("[BACKTEST_PNL] PORTFOLIO: cumulative_total_pnl=" + std::to_string(cumulative_total_pnl_) +
             ", portfolio_value=" + std::to_string(current_portfolio_value_));
    
    result.success = true;
    return result;
}

// ============================================================================
// Calculate PnL for a single position
// ============================================================================
BacktestPnLManager::PositionPnLResult BacktestPnLManager::calculate_position_pnl(
    const std::string& symbol,
    double quantity,
    double previous_close,
    double current_close) {
    
    PositionPnLResult result;
    result.symbol = symbol;
    result.quantity = quantity;
    result.previous_close = previous_close;
    result.current_close = current_close;
    
    // Get point value
    double point_value = get_point_value(symbol);
    result.point_value = point_value;
    
    // Calculate price change
    double price_change = current_close - previous_close;
    
    // Calculate PnL: quantity * (current - previous) * point_value
    double daily_pnl = quantity * price_change * point_value;
    result.daily_pnl = daily_pnl;
    result.valid = true;
    
    // Comprehensive debug logging
    log_info("[BACKTEST_PNL] CALC: " + symbol +
             " | date=" + current_date_str_ +
             " | qty=" + std::to_string(quantity) +
             " | prev_close=" + std::to_string(previous_close) +
             " | curr_close=" + std::to_string(current_close) +
             " | price_change=" + std::to_string(price_change) +
             " | point_value=" + std::to_string(point_value) +
             " | FORMULA: " + std::to_string(quantity) + " * (" +
             std::to_string(current_close) + " - " + std::to_string(previous_close) + ") * " +
             std::to_string(point_value) + " = " + std::to_string(daily_pnl));
    
    return result;
}

// ============================================================================
// Get point value multiplier for a symbol
// ============================================================================
double BacktestPnLManager::get_point_value(const std::string& symbol) const {
    std::string base_symbol = extract_base_symbol(symbol);
    
    // Try InstrumentRegistry first
    if (registry_.has_instrument(base_symbol)) {
        try {
            auto instrument = registry_.get_instrument(base_symbol);
            if (instrument) {
                double multiplier = instrument->get_multiplier();
                if (multiplier > 0) {
                    log_debug("[BACKTEST_PNL] POINT_VALUE: " + symbol + 
                              " (base=" + base_symbol + ") from registry = " + 
                              std::to_string(multiplier));
                    return multiplier;
                }
            }
        } catch (const std::exception& e) {
            log_warn("[BACKTEST_PNL] POINT_VALUE: Registry lookup failed for " + symbol +
                     ": " + e.what());
        }
    }
    
    // Fall back to known values
    double fallback = get_fallback_multiplier(base_symbol);
    if (fallback > 0) {
        log_debug("[BACKTEST_PNL] POINT_VALUE: " + symbol + 
                  " (base=" + base_symbol + ") from fallback = " + 
                  std::to_string(fallback));
        return fallback;
    }
    
    // Last resort
    log_warn("[BACKTEST_PNL] POINT_VALUE: " + symbol + 
             " - NO VALUE FOUND, using 1.0 (THIS MAY CAUSE INCORRECT PNL)");
    return 1.0;
}

// ============================================================================
// Extract base symbol (remove .v./.c. suffix)
// ============================================================================
std::string BacktestPnLManager::extract_base_symbol(const std::string& symbol) const {
    std::string base = symbol;
    
    size_t pos = symbol.find(".v.");
    if (pos != std::string::npos) {
        base = symbol.substr(0, pos);
    }
    
    pos = base.find(".c.");
    if (pos != std::string::npos) {
        base = base.substr(0, pos);
    }
    
    return base;
}

// ============================================================================
// Get fallback multiplier for known symbols
// Same values as LivePnLManager for consistency
// ============================================================================
double BacktestPnLManager::get_fallback_multiplier(const std::string& symbol) const {
    // Equity Index Futures
    if (symbol.find("MNQ") != std::string::npos || symbol.find("NQ") != std::string::npos) 
        return 2.0;      // Micro/Mini E-mini Nasdaq-100: 0.5 / 0.25
    if (symbol.find("MES") != std::string::npos || symbol.find("ES") != std::string::npos) 
        return 5.0;      // Micro/Mini E-mini S&P 500: 1.25 / 0.25
    if (symbol.find("MYM") != std::string::npos || symbol.find("YM") != std::string::npos) 
        return 0.5;      // Micro/Mini E-mini Dow: 0.5 / 1
    if (symbol.find("M2K") != std::string::npos || symbol.find("RTY") != std::string::npos) 
        return 5.0;      // E-mini Russell 2000: 0.5 / 0.1
    
    // Energy Futures
    if (symbol.find("MCL") != std::string::npos) 
        return 100.0;    // Micro Crude Oil: 1 / 0.01
    if (symbol.find("CL") != std::string::npos) 
        return 1000.0;   // Crude Oil: 10 / 0.01
    if (symbol.find("RB") != std::string::npos) 
        return 42000.0;  // RBOB Gasoline: 4.2 / 0.0001
    if (symbol.find("NG") != std::string::npos) 
        return 10000.0;  // Natural Gas: 10 / 0.001
    
    // Metals Futures
    if (symbol.find("MGC") != std::string::npos) 
        return 100.0;    // Micro Gold: 1 / 0.01
    if (symbol.find("GC") != std::string::npos) 
        return 100.0;    // Gold: 10 / 0.1
    if (symbol.find("SIL") != std::string::npos) 
        return 1000.0;   // Micro Silver: 5 / 0.005
    if (symbol.find("SI") != std::string::npos) 
        return 5000.0;   // Silver: 25 / 0.005
    if (symbol.find("HG") != std::string::npos) 
        return 25000.0;  // Copper: 12.5 / 0.0005
    if (symbol.find("PL") != std::string::npos) 
        return 50.0;     // Platinum: 5 / 0.1
    
    // Currency Futures
    if (symbol.find("6A") != std::string::npos) 
        return 100000.0;  // AUD: 10 / 0.0001
    if (symbol.find("6C") != std::string::npos) 
        return 100000.0;  // CAD: 10 / 0.0001
    if (symbol.find("6E") != std::string::npos || symbol.find("M6E") != std::string::npos) 
        return 125000.0;  // EUR: 12.5 / 0.0001
    if (symbol.find("6J") != std::string::npos) 
        return 12500000.0; // JPY: 12.5 / 0.000001
    if (symbol.find("6M") != std::string::npos) 
        return 500000.0;  // MXN: 5 / 0.00001
    if (symbol.find("6N") != std::string::npos) 
        return 100000.0;  // NZD: 10 / 0.0001
    if (symbol.find("6S") != std::string::npos || symbol.find("MSF") != std::string::npos) 
        return 125000.0;  // CHF: 12.5 / 0.0001
    if (symbol.find("6B") != std::string::npos || symbol.find("M6B") != std::string::npos) 
        return 62500.0;   // GBP: 6.25 / 0.0001
    
    // Agricultural Futures
    if (symbol.find("ZC") != std::string::npos) 
        return 50.0;     // Corn: 12.5 / 0.25
    if (symbol.find("ZS") != std::string::npos || symbol.find("YK") != std::string::npos) 
        return 50.0;     // Soybeans: 12.5 / 0.25
    if (symbol.find("ZW") != std::string::npos || symbol.find("YW") != std::string::npos) 
        return 50.0;     // Wheat: 12.5 / 0.25
    if (symbol.find("ZM") != std::string::npos) 
        return 100.0;    // Soybean Meal: 10 / 0.1
    if (symbol.find("ZL") != std::string::npos) 
        return 600.0;    // Soybean Oil: 6 / 0.01
    if (symbol.find("ZR") != std::string::npos) 
        return 20.0;     // Rough Rice: 10 / 0.5
    if (symbol.find("KE") != std::string::npos) 
        return 50.0;     // KC Wheat: 12.5 / 0.25
    if (symbol.find("GF") != std::string::npos) 
        return 500.0;    // Feeder Cattle: 50 / 0.025
    if (symbol.find("HE") != std::string::npos) 
        return 400.0;    // Lean Hogs: 10 / 0.025
    if (symbol.find("LE") != std::string::npos) 
        return 400.0;    // Live Cattle: 10 / 0.025
    
    // Interest Rate Futures
    if (symbol.find("ZN") != std::string::npos) 
        return 1000.0;   // 10-Year T-Note
    if (symbol.find("ZB") != std::string::npos) 
        return 1000.0;   // 30-Year T-Bond
    if (symbol.find("ZF") != std::string::npos) 
        return 1000.0;   // 5-Year T-Note
    if (symbol.find("ZT") != std::string::npos) 
        return 2000.0;   // 2-Year T-Note
    if (symbol.find("UB") != std::string::npos) 
        return 1000.0;   // Ultra T-Bond
    
    // VIX Futures
    if (symbol.find("VX") != std::string::npos) 
        return 1000.0;   // VIX: 1000 / 1
    
    // Unknown symbol
    return 0.0;
}

// ============================================================================
// Update previous close prices for next day's calculation
// ============================================================================
void BacktestPnLManager::update_previous_closes(
    const std::unordered_map<std::string, double>& close_prices) {
    
    log_debug("[BACKTEST_PNL] PREV_CLOSE: Updating " + 
              std::to_string(close_prices.size()) + " previous close prices");
    
    for (const auto& [symbol, close] : close_prices) {
        previous_close_prices_[symbol] = close;
        log_debug("[BACKTEST_PNL] PREV_CLOSE: " + symbol + " = " + std::to_string(close));
    }
}

void BacktestPnLManager::set_previous_close(const std::string& symbol, double close_price) {
    previous_close_prices_[symbol] = close_price;
    log_debug("[BACKTEST_PNL] PREV_CLOSE: Set " + symbol + " = " + std::to_string(close_price));
}

double BacktestPnLManager::get_previous_close(const std::string& symbol) const {
    auto it = previous_close_prices_.find(symbol);
    return (it != previous_close_prices_.end()) ? it->second : 0.0;
}

bool BacktestPnLManager::has_previous_close(const std::string& symbol) const {
    return previous_close_prices_.find(symbol) != previous_close_prices_.end();
}

// ============================================================================
// Reset functions
// ============================================================================
void BacktestPnLManager::reset() {
    log_info("[BACKTEST_PNL] RESET: Full reset - clearing all state");
    
    previous_close_prices_.clear();
    position_daily_pnl_.clear();
    position_cumulative_pnl_.clear();
    daily_total_pnl_ = 0.0;
    cumulative_total_pnl_ = 0.0;
    current_portfolio_value_ = initial_capital_;
    current_date_str_.clear();
}

void BacktestPnLManager::reset_daily() {
    log_debug("[BACKTEST_PNL] RESET: Daily reset");
    position_daily_pnl_.clear();
    daily_total_pnl_ = 0.0;
}

// ============================================================================
// Getters for position PnL
// ============================================================================
double BacktestPnLManager::get_position_daily_pnl(const std::string& symbol) const {
    auto it = position_daily_pnl_.find(symbol);
    return (it != position_daily_pnl_.end()) ? it->second : 0.0;
}

double BacktestPnLManager::get_position_cumulative_pnl(const std::string& symbol) const {
    auto it = position_cumulative_pnl_.find(symbol);
    return (it != position_cumulative_pnl_.end()) ? it->second : 0.0;
}

// ============================================================================
// Utility functions
// ============================================================================
std::string BacktestPnLManager::format_date(const Timestamp& ts) const {
    auto time_t = std::chrono::system_clock::to_time_t(ts);
    std::tm tm;
    // Use gmtime_r for thread safety (POSIX)
#ifdef _WIN32
    gmtime_s(&tm, &time_t);
#else
    gmtime_r(&time_t, &tm);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d");
    return ss.str();
}

void BacktestPnLManager::log_debug(const std::string& message) const {
    if (debug_enabled_) {
        DEBUG(message);
    }
}

void BacktestPnLManager::log_info(const std::string& message) const {
    INFO(message);
}

void BacktestPnLManager::log_warn(const std::string& message) const {
    WARN(message);
}

} // namespace backtest
} // namespace trade_ngin

