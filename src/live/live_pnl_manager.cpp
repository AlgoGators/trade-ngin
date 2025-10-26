#include "trade_ngin/live/live_pnl_manager.hpp"
// #include "trade_ngin/core/instrument_registry.hpp"  // TODO: Add when available
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

    for (const auto& position : previous_positions) {
        const std::string& symbol = position.symbol;
        double quantity = position.quantity.as_double();

        // Get Day T-2 close (previous close for Day T-1)
        auto t2_it = t2_close_prices.find(symbol);
        if (t2_it == t2_close_prices.end()) {
            WARN("No T-2 close price for " + symbol + ", skipping finalization");
            continue;
        }
        double day_t2_close = t2_it->second;

        // Get Day T-1 close (current close for Day T-1)
        double day_t1_close = day_t2_close;  // Default to T-2 if T-1 not available
        auto t1_it = t1_close_prices.find(symbol);
        if (t1_it != t1_close_prices.end()) {
            day_t1_close = t1_it->second;
        } else {
            WARN("No T-1 close price for " + symbol + ", using T-2 close");
        }

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

        // Create finalized position with realized PnL
        Position finalized_pos = position;
        finalized_pos.realized_pnl = Decimal(yesterday_position_pnl);
        finalized_pos.unrealized_pnl = Decimal(0.0);  // Always 0 for futures
        // finalized_pos.market_price = Decimal(day_t1_close);  // TODO: Add if field exists
        result.finalized_positions.push_back(finalized_pos);
    }

    // Calculate finalized portfolio value
    double net_pnl = calculate_net_pnl(total_finalized_pnl, commissions);
    result.finalized_daily_pnl = net_pnl;
    result.finalized_portfolio_value = calculate_portfolio_value(previous_portfolio_value, net_pnl);

    INFO("Day T-1 finalization complete: Total PnL=" + std::to_string(total_finalized_pnl) +
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
    // if (!instrument_registry_) {
        // Default point values if no registry
        // This should match the values used in live_trend.cpp
        if (symbol.find("NQ") != std::string::npos) return 20.0;
        if (symbol.find("YM") != std::string::npos) return 5.0;
        if (symbol.find("RTY") != std::string::npos) return 50.0;
        if (symbol.find("CL") != std::string::npos) return 1000.0;
        if (symbol.find("RB") != std::string::npos) return 42000.0;
        if (symbol.find("HG") != std::string::npos) return 25000.0;
        if (symbol.find("GC") != std::string::npos) return 100.0;
        if (symbol.find("SI") != std::string::npos) return 5000.0;
        if (symbol.find("6") == 0) return 100000.0;  // Currency futures
        if (symbol.find("ZC") != std::string::npos) return 50.0;
        if (symbol.find("ZS") != std::string::npos) return 50.0;
        if (symbol.find("ZM") != std::string::npos) return 100.0;
        if (symbol.find("ZL") != std::string::npos) return 60000.0;
        if (symbol.find("ZW") != std::string::npos) return 50.0;
        if (symbol.find("ZR") != std::string::npos) return 2000.0;

        WARN("Using default point value 1.0 for unknown symbol: " + symbol);
        return 1.0;
    // }

    // TODO: Get from registry when available
    // auto instrument = instrument_registry_->get_futures_instrument(symbol);
    // if (instrument) {
    //     return instrument->point_value;
    // }

    // WARN("Symbol not found in instrument registry: " + symbol);
    // return 1.0;
}
} // namespace trade_ngin
