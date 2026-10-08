#include "trade_ngin/live/live_pnl_manager.hpp"
#include "trade_ngin/core/logger.hpp"
#include <cmath>

namespace trade_ngin {

Result<LivePnLManager::FinalizationResult> LivePnLManager::finalize_previous_day(
    const std::vector<Position>& previous_positions,
    const std::unordered_map<std::string, double>& t1_close_prices,
    const std::unordered_map<std::string, double>& t2_close_prices,
    double previous_portfolio_value,
    double commissions,
    UnrealizedPolicy unrealized_policy) {

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
            Position finalized_pos = position;
            result.position_realized_pnl[symbol] = 0.0;
            if (unrealized_policy == UnrealizedPolicy::MARK_TO_MARKET) {
                // E2-F19 R-1 / R-2 (cash book): a symbol with no T-1 print -- a halt, a
                // thin name, a feed gap -- realized nothing today (its trade flow is what
                // the day's own run recorded, kept as-is) and is still worth its last
                // known mark. Writing 0/0 here erased a real level for a day and had it
                // reappear the next session as a phantom jump in daily unrealized.
                finalized_pos.realized_pnl = position.realized_pnl;
                finalized_pos.unrealized_pnl = position.unrealized_pnl;
                result.finalized_unrealized_pnl += finalized_pos.unrealized_pnl.as_double();
                WARN("No T-1 close price for " + symbol +
                     ": carrying its last known mark (unrealized " +
                     std::to_string(position.unrealized_pnl.as_double()) +
                     ") and its recorded realized. A symbol missing a print on a trading "
                     "day is a halt or a feed gap -- investigate if it persists.");
            } else {
                WARN("No T-1 close price for " + symbol + ", recording position with 0 PnL (no Day T-1 data available)");
                // Settled book: no settlement move can be booked without the print.
                finalized_pos.realized_pnl = Decimal(0.0);
                finalized_pos.unrealized_pnl = Decimal(0.0);
            }
            result.finalized_positions.push_back(finalized_pos);
            INFO("Added " + symbol + " to finalized positions (position continuity maintained)");
            continue;
        }
        double day_t1_close = t1_it->second;

        // Get Day T-2 close (previous close for Day T-1)
        // T-2 can fall back to older data if needed (e.g., skip weekends for agriculture futures)
        auto t2_it = t2_close_prices.find(symbol);
        if (t2_it == t2_close_prices.end()) {
            Position finalized_pos = position;
            result.position_realized_pnl[symbol] = 0.0;
            if (unrealized_policy == UnrealizedPolicy::MARK_TO_MARKET) {
                // Cash book with a T-1 close but no T-2 close: the mark IS computable from
                // the cost basis, and realized is the day's trade flow (R-1 / R-2).
                double avg_price = position.average_price.as_double();
                finalized_pos.realized_pnl = position.realized_pnl;
                finalized_pos.unrealized_pnl =
                    (avg_price > 0.0 && quantity != 0.0)
                        ? Decimal(quantity * (day_t1_close - avg_price) * get_point_value(symbol))
                        : position.unrealized_pnl;
                result.finalized_unrealized_pnl += finalized_pos.unrealized_pnl.as_double();
                WARN("No T-2 close price for " + symbol +
                     ": no settlement move to report; marked at T-1 against the cost basis.");
            } else {
                WARN("No T-2 close price for " + symbol + ", recording position with 0 PnL (T-2 data unavailable)");
                finalized_pos.realized_pnl = Decimal(0.0);
                finalized_pos.unrealized_pnl = Decimal(0.0);
            }
            result.finalized_positions.push_back(finalized_pos);
            INFO("Added " + symbol + " to finalized positions (position continuity maintained)");
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

        // Create finalized position with realized PnL.
        //
        // E2-F19 R-1: what lands in the ROW's realized_pnl follows the settlement model.
        // SETTLED (futures): the settlement move IS the day's realized. MARK_TO_MARKET
        // (equities): the row keeps the trade realized the day's own run recorded; the
        // move is a mark and belongs in unrealized_pnl below. The aggregate fields
        // (position_realized_pnl, finalized_daily_pnl) still carry the move in both
        // models -- that is what the T-1 live_results update and the equity curve read.
        Position finalized_pos = position;
        finalized_pos.realized_pnl =
            (unrealized_policy == UnrealizedPolicy::MARK_TO_MARKET)
                ? position.realized_pnl
                : Decimal(yesterday_position_pnl);

        // Unrealized P&L, per the caller's settlement model -- see UnrealizedPolicy.
        //
        // SETTLED (futures, the default): the position was entered at close(T-1) and is
        // being settled here at close(T); that whole move is booked as realized just
        // above, so there is no unrealized component. Writing one would record the same
        // settlement move twice, because on a futures row average_price IS day_t2_close
        // and the expression below reduces exactly to the realized formula (E2-F3).
        //
        // MARK_TO_MARKET (equities): average_price is a true weighted cost basis, so the
        // expression is the genuine unrealized P&L.
        if (unrealized_policy == UnrealizedPolicy::MARK_TO_MARKET) {
            double avg_price = position.average_price.as_double();
            finalized_pos.unrealized_pnl =
                (avg_price > 0.0 && quantity != 0.0)
                    ? Decimal(quantity * (day_t1_close - avg_price) * point_value)
                    : Decimal(0.0);
            // E2-F1: carry the same figure into the aggregate so the row-sums-to-aggregate
            // invariant (protocol L5) holds by construction. Deliberately accumulated inside
            // the MARK_TO_MARKET branch only -- under SETTLED this stays 0.0 for futures
            // without needing a second guard anywhere.
            result.finalized_unrealized_pnl += finalized_pos.unrealized_pnl.as_double();
        } else {
            finalized_pos.unrealized_pnl = Decimal(0.0);
        }

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

        // Track unrealized PnL from cost basis (average_price). Shared rule -- the
        // equity runner persists per-row unrealized_pnl through the same function, so
        // the aggregate here and the DB rows cannot drift apart.
        double avg_price = position.average_price.as_double();
        if (avg_price > 0.0 && quantity != 0.0) {
            position_unrealized_pnl_[symbol] =
                unrealized_from_cost_basis(quantity, avg_price, current_price, point_value);
        }

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

    double total_unrealized = 0.0;
    for (const auto& [symbol, pnl] : position_unrealized_pnl_) {
        total_unrealized += pnl;
    }
    snapshot.unrealized_pnl = total_unrealized;

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

    // E2-F38 / BA-7: the fallback table below is a SUBSTRING match over futures
    // contract codes, so it must only be consulted for futures. Real equity
    // tickers collide with it: AAPL contains "PL" (platinum, 50), LEN contains
    // "LE" (live cattle, 40000), ZM is soybean meal (100), UBER contains "UB"
    // (ultra T-bond, 1000), HES contains "ES" (5). Every one of those is a P&L
    // report wrong by orders of magnitude on a position that merely failed to
    // register.
    //
    // A share is a share: one unit, point value 1.0. There is nothing to guess.
    if (asset_type_ != AssetType::FUTURE) {
        WARN("No registry entry for " + symbol + "; it is not a futures symbol, so its "
             "point value is 1.0. The futures fallback table is a substring match and "
             "would have guessed a contract multiplier for it.");
        return 1.0;
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
    // Fallback multipliers calculated as: minimum_price_fluctuation / tick_size
    // Source: Contract specifications as of 2025
    
    // Equity Index Futures
    if (symbol.find("MNQ") != std::string::npos || symbol.find("NQ") != std::string::npos) 
        return 2.0;      // Micro/Mini E-mini Nasdaq-100: 0.5 / 0.25
    if (symbol.find("MES") != std::string::npos || symbol.find("ES") != std::string::npos) 
        return 5.0;      // Micro/Mini E-mini S&P 500: 1.25 / 0.25
    if (symbol.find("MYM") != std::string::npos || symbol.find("YM") != std::string::npos) 
        return 0.5;      // Micro/Mini E-mini Dow: 0.5 / 1
    if (symbol.find("RTY") != std::string::npos) 
        return 5.0;      // E-mini Russell 2000: 0.5 / 0.1
    
    // Energy Futures
    if (symbol.find("CL") != std::string::npos) 
        return 1000.0;   // Crude Oil: 10 / 0.01
    if (symbol.find("RB") != std::string::npos) 
        return 42000.0;  // RBOB Gasoline: 4.2 / 0.0001
    
    // Metals Futures
    if (symbol.find("GC") != std::string::npos) 
        return 1000.0;   // Gold: 10 / 0.01
    if (symbol.find("SI") != std::string::npos) 
        return 5000.0;   // Silver: 25 / 0.005
    if (symbol.find("HG") != std::string::npos) 
        return 25000.0;  // Copper: 12.5 / 0.0005
    if (symbol.find("PL") != std::string::npos) 
        return 50.0;     // Platinum: 5 / 0.1
    
    // Currency Futures
    if (symbol.find("6C") != std::string::npos) 
        return 100000.0;  // CAD: 5 / 0.00005
    if (symbol.find("6E") != std::string::npos || symbol.find("M6E") != std::string::npos) 
        return 125000.0;  // EUR: 6.25 / 0.00005
    if (symbol.find("6J") != std::string::npos) 
        return 12500000.0; // JPY: 6.25 / 0.0000005
    if (symbol.find("6M") != std::string::npos) 
        return 500000.0;  // MXN: 5 / 0.00001
    if (symbol.find("6N") != std::string::npos) 
        return 100000.0;  // NZD: 5 / 0.00005
    if (symbol.find("6S") != std::string::npos || symbol.find("MSF") != std::string::npos) 
        return 125000.0;  // CHF: 6.25 / 0.00005
    if (symbol.find("6B") != std::string::npos || symbol.find("M6B") != std::string::npos) 
        return 62500.0;   // GBP: 6.25 / 0.0001
    
    // Agricultural Futures
    if (symbol.find("ZC") != std::string::npos) 
        return 5000.0;   // Corn: 12.5 / 0.0025
    if (symbol.find("ZS") != std::string::npos || symbol.find("YK") != std::string::npos) 
        return 5000.0;   // Soybeans: 12.5 / 0.0025
    if (symbol.find("ZW") != std::string::npos || symbol.find("YW") != std::string::npos) 
        return 5000.0;   // Wheat: 12.5 / 0.0025
    if (symbol.find("ZM") != std::string::npos) 
        return 100.0;    // Soybean Meal: 10 / 0.1
    if (symbol.find("ZL") != std::string::npos) 
        return 60000.0;  // Soybean Oil: 6 / 0.0001
    if (symbol.find("ZR") != std::string::npos) 
        return 2000.0;   // Rough Rice: 10 / 0.005
    if (symbol.find("KE") != std::string::npos) 
        return 5000.0;   // KC Wheat: 12.5 / 0.0025
    if (symbol.find("GF") != std::string::npos) 
        return 40000.0;  // Feeder Cattle: 10 / 0.00025
    if (symbol.find("HE") != std::string::npos) 
        return 40000.0;  // Lean Hogs: 10 / 0.00025
    if (symbol.find("LE") != std::string::npos) 
        return 40000.0;  // Live Cattle: 10 / 0.00025
    
    // Interest Rate Futures
    if (symbol.find("ZN") != std::string::npos) 
        return 1000.0;   // 10-Year T-Note: 15.625 / 0.015625
    if (symbol.find("UB") != std::string::npos) 
        return 1000.0;   // Ultra T-Bond: 31.25 / 0.03125
    
    // Unknown symbol
    return 0.0;
}
} // namespace trade_ngin
