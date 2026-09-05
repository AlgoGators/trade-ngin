#include "trade_ngin/live/live_pnl_manager.hpp"
#include "trade_ngin/instruments/contract_multiplier.hpp"
#include "trade_ngin/core/logger.hpp"
#include <cmath>

namespace trade_ngin {

Result<LivePnLManager::FinalizationResult> LivePnLManager::finalize_previous_day(
    const std::vector<Position>& previous_positions,
    const std::unordered_map<std::string, double>& t1_close_prices,
    const std::unordered_map<std::string, double>& t2_close_prices,
    double previous_portfolio_value,
    double commissions) {

    FinalizationResult result;

    if (previous_positions.empty()) {
        INFO("No positions to finalize for Day T-1");
        result.success = true;
        return Result<FinalizationResult>(result);
    }

    if (t2_close_prices.empty()) {
        return make_error<FinalizationResult>(ErrorCode::INVALID_DATA, 
            "Cannot finalize Day T-1: No T-2 close prices available");
    }

    double total_finalized_pnl = 0.0;

    INFO("Finalizing " + std::to_string(previous_positions.size()) + " positions for Day T-1");
    INFO("DEBUG FINALIZATION: Starting finalize_previous_day()");

    for (const auto& position : previous_positions) {
        const std::string& symbol = position.symbol;
        double quantity = position.quantity.as_double();

        // Get Day T-1 close (current close for Day T-1)
        // T-1 data is REQUIRED - if missing, add position with 0 PnL to maintain continuity
        auto t1_it = t1_close_prices.find(symbol);
        if (t1_it == t1_close_prices.end()) {
            WARN("No T-1 close price for " + symbol + ", recording position with 0 PnL (no Day T-1 data available)");
            
            // Still add this position to database with 0 PnL to maintain position continuity
            Position finalized_pos = position;
            finalized_pos.realized_pnl = Decimal(0.0);
            finalized_pos.unrealized_pnl = Decimal(0.0);
            result.position_realized_pnl[symbol] = 0.0;
            result.finalized_positions.push_back(finalized_pos);
            
            INFO("Added " + symbol + " to finalized positions with 0 PnL (position continuity maintained)");
            continue;
        }
        double day_t1_close = t1_it->second;

        // Get Day T-2 close (previous close for Day T-1)
        // T-2 can fall back to older data if needed (e.g., skip weekends for agriculture futures)
        auto t2_it = t2_close_prices.find(symbol);
        if (t2_it == t2_close_prices.end()) {
            WARN("No T-2 close price for " + symbol + ", recording position with 0 PnL (T-2 data unavailable)");
            
            // Still add this position to database with 0 PnL to maintain position continuity
            Position finalized_pos = position;
            finalized_pos.realized_pnl = Decimal(0.0);
            finalized_pos.unrealized_pnl = Decimal(0.0);
            result.position_realized_pnl[symbol] = 0.0;
            result.finalized_positions.push_back(finalized_pos);
            
            INFO("Added " + symbol + " to finalized positions with 0 PnL (position continuity maintained)");
            continue;
        }
        double day_t2_close = t2_it->second;

        // Get point value for the symbol
        double point_value = get_point_value(symbol);

        // Calculate Day T-1 PnL
        double yesterday_position_pnl = calculate_daily_pnl(
            quantity,
            day_t2_close,
            day_t1_close,
            point_value
        );

        INFO("Day T-1 finalization for " + symbol +
             ": qty=" + std::to_string(quantity) +
             ", T-2 close=" + std::to_string(day_t2_close) +
             ", T-1 close=" + std::to_string(day_t1_close) +
             ", point_value=" + std::to_string(point_value) +
             ", PnL=" + std::to_string(yesterday_position_pnl));

        // Store finalized PnL
        result.position_realized_pnl[symbol] = yesterday_position_pnl;
        total_finalized_pnl += yesterday_position_pnl;

        INFO("DEBUG FINALIZATION: Added " + symbol + " PnL=" + std::to_string(yesterday_position_pnl) +
             " to running sum, new total=" + std::to_string(total_finalized_pnl));

        // Create finalized position with realized PnL
        Position finalized_pos = position;
        finalized_pos.realized_pnl = Decimal(yesterday_position_pnl);
        finalized_pos.unrealized_pnl = Decimal(0.0);  // Always 0 for futures
        // finalized_pos.market_price = Decimal(day_t1_close);  // TODO: Add if field exists
        result.finalized_positions.push_back(finalized_pos);
    }

    // Calculate finalized portfolio value
    // NOTE: Return GROSS PnL in finalized_daily_pnl (commissions will be subtracted elsewhere)
    double net_pnl = calculate_net_pnl(total_finalized_pnl, commissions);
    result.finalized_daily_pnl = total_finalized_pnl;  // GROSS PnL

    INFO("DEBUG FINALIZATION: Final total_finalized_pnl=" + std::to_string(total_finalized_pnl));
    INFO("DEBUG FINALIZATION: Result.finalized_daily_pnl (GROSS)=" + std::to_string(result.finalized_daily_pnl));
    result.finalized_portfolio_value = calculate_portfolio_value(previous_portfolio_value, net_pnl);

    INFO("Day T-1 finalization complete: Total PnL (Gross)=" + std::to_string(total_finalized_pnl) +
         ", Net PnL=" + std::to_string(net_pnl) +
         ", Portfolio Value=" + std::to_string(result.finalized_portfolio_value));

    result.success = true;
    return Result<FinalizationResult>(result);
}

Result<void> LivePnLManager::calculate_position_pnls(
    const std::vector<Position>& positions,
    const std::unordered_map<std::string, double>& current_prices,
    const std::unordered_map<std::string, double>& previous_prices) {

    reset_daily_tracking();

    for (const auto& position : positions) {
        const std::string& symbol = position.symbol;
        double quantity = position.quantity.as_double();

        // Get current price
        auto curr_it = current_prices.find(symbol);
        if (curr_it == current_prices.end()) {
            WARN("No current price for " + symbol + ", skipping PnL calculation");
            continue;
        }
        double current_price = curr_it->second;

        // Get previous price
        auto prev_it = previous_prices.find(symbol);
        if (prev_it == previous_prices.end()) {
            WARN("No previous price for " + symbol + ", skipping PnL calculation");
            continue;
        }
        double previous_price = prev_it->second;

        // Get point value
        double point_value = get_point_value(symbol);

        // Calculate daily PnL
        double daily_pnl = calculate_daily_pnl(
            quantity,
            previous_price,
            current_price,
            point_value
        );

        // Update tracking
        position_daily_pnl_[symbol] = daily_pnl;
        cumulative_daily_pnl_ += daily_pnl;

        DEBUG("Position PnL for " + symbol +
              ": qty=" + std::to_string(quantity) +
              ", prev=" + std::to_string(previous_price) +
              ", curr=" + std::to_string(current_price) +
              ", daily_pnl=" + std::to_string(daily_pnl));
    }

    INFO("Calculated PnL for " + std::to_string(positions.size()) +
         " positions, Total Daily PnL=" + std::to_string(cumulative_daily_pnl_));

    return Result<void>();
}

Result<void> LivePnLManager::update_position_pnl(
    const std::string& symbol,
    double daily_pnl,
    double realized_pnl) {

    position_daily_pnl_[symbol] = daily_pnl;

    if (realized_pnl != 0.0) {
        position_realized_pnl_[symbol] = realized_pnl;
    }

    // Recalculate totals
    cumulative_daily_pnl_ = 0.0;
    for (const auto& [sym, pnl] : position_daily_pnl_) {
        cumulative_daily_pnl_ += pnl;
    }

    return Result<void>();
}

Result<LivePnLManager::PnLSnapshot> LivePnLManager::get_current_snapshot() const {
    PnLSnapshot snapshot;
    snapshot.daily_pnl = cumulative_daily_pnl_;
    snapshot.total_pnl = cumulative_total_pnl_;

    // Calculate realized and unrealized
    double total_realized = 0.0;
    for (const auto& [symbol, pnl] : position_realized_pnl_) {
        total_realized += pnl;
    }
    snapshot.realized_pnl = total_realized;
    snapshot.unrealized_pnl = 0.0;  // Always 0 for futures

    // Portfolio value needs to be calculated externally
    snapshot.portfolio_value = initial_capital_ + cumulative_total_pnl_;
    snapshot.timestamp = std::chrono::system_clock::now();

    return Result<PnLSnapshot>(snapshot);
}

double LivePnLManager::get_position_daily_pnl(const std::string& symbol) const {
    auto it = position_daily_pnl_.find(symbol);
    return (it != position_daily_pnl_.end()) ? it->second : 0.0;
}

double LivePnLManager::get_position_realized_pnl(const std::string& symbol) const {
    auto it = position_realized_pnl_.find(symbol);
    return (it != position_realized_pnl_.end()) ? it->second : 0.0;
}

double LivePnLManager::get_point_value(const std::string& symbol) const {
    // Extract base symbol (remove .v./.c. suffix)
    std::string base_symbol = symbol;
    if (symbol.find(".v.") != std::string::npos) {
        base_symbol = symbol.substr(0, symbol.find(".v."));
    }
    if (symbol.find(".c.") != std::string::npos) {
        base_symbol = symbol.substr(0, symbol.find(".c."));
    }

    // Try to get from InstrumentRegistry first
    if (registry_.has_instrument(base_symbol)) {
        try {
            auto instrument = registry_.get_instrument(base_symbol);
            if (instrument) {
                double multiplier = instrument->get_multiplier();
                if (multiplier > 0) {
                    DEBUG("Retrieved point value from registry for " + symbol + ": " + 
                          std::to_string(multiplier));
                    return multiplier;
                }
            }
        } catch (const std::exception& e) {
            WARN("Failed to get multiplier from registry for " + symbol + ": " + e.what() +
                 ", using fallback");
        }
    }

    // Fall back to known values if registry lookup fails
    double fallback = get_fallback_multiplier(base_symbol);
    if (fallback > 0) {
        DEBUG("Using fallback multiplier for " + symbol + ": " + std::to_string(fallback));
        return fallback;
    }

    // Last resort - warn and use 1.0
    WARN("No multiplier found for " + symbol + " in registry or fallback, using 1.0");
    return 1.0;
}

double LivePnLManager::get_fallback_multiplier(const std::string& symbol) const {
    // One table now, in instruments/contract_multiplier.cpp, which states the
    // quote convention beside every contract size. This function used to carry
    // its own copy: it had corn at 5,000 and live cattle at 40,000, which are
    // the contract sizes in bushels and pounds, not the value of a point.
    //
    // 0.0 still means "unknown" -- the caller warns and falls back to 1.0.
    return fallback_price_multiplier(symbol).value_or(0.0);
}

} // namespace trade_ngin
