#include "trade_ngin/live/margin_manager.hpp"
#include "trade_ngin/core/logger.hpp"
#include <cmath>
#include <iostream>
#include <iomanip>

namespace trade_ngin {

Result<MarginManager::MarginMetrics> MarginManager::calculate_margin_requirements(
    const std::unordered_map<std::string, Position>& positions,
    const std::unordered_map<std::string, double>& market_prices,
    double portfolio_value) {

    MarginMetrics metrics;

    INFO("Calculating margin requirements for " + std::to_string(positions.size()) + " positions");

    // Process each position
    for (const auto& [symbol, position] : positions) {
        double quantity = position.quantity.as_double();

        // Skip zero positions
        if (std::abs(quantity) < 1e-6) {
            continue;
        }

        metrics.active_positions++;

        // Get market price (use Day T-1 close)
        double market_price = position.average_price.as_double();
        auto price_it = market_prices.find(symbol);
        if (price_it != market_prices.end()) {
            market_price = price_it->second;
        } else {
            WARN("No market price for " + symbol + ", using average price");
        }

        // Calculate margin for this position
        auto margin_result = calculate_position_margin(symbol, quantity, market_price);
        if (margin_result.is_error()) {
            return make_error<MarginMetrics>(
                ErrorCode::INVALID_RISK_CALCULATION,
                "Failed to calculate margin for " + symbol + ": " +
                std::string(margin_result.error()->what()));
        }

        auto [initial_margin, maintenance_margin] = margin_result.value();
        metrics.total_posted_margin += initial_margin;
        metrics.maintenance_requirement += maintenance_margin;
        metrics.symbol_margins[symbol] = initial_margin;

        // Calculate notional for this position
        auto notional_result = calculate_position_notional(symbol, quantity, market_price);
        if (notional_result.is_error()) {
            return make_error<MarginMetrics>(
                ErrorCode::INVALID_RISK_CALCULATION,
                "Failed to calculate notional for " + symbol + ": " +
                std::string(notional_result.error()->what()));
        }

        double signed_notional = notional_result.value();
        metrics.net_notional += signed_notional;
        metrics.gross_notional += std::abs(signed_notional);
        metrics.symbol_notionals[symbol] = signed_notional;

        DEBUG("Position " + symbol + ": qty=" + std::to_string(quantity) +
              ", price=" + std::to_string(market_price) +
              ", notional=" + std::to_string(signed_notional) +
              ", margin=" + std::to_string(initial_margin));
    }

    // Calculate derived metrics
    metrics.gross_leverage = calculate_gross_leverage(metrics.gross_notional, portfolio_value);
    metrics.equity_to_margin_ratio = calculate_equity_to_margin_ratio(
        metrics.gross_notional, metrics.total_posted_margin);
    metrics.margin_cushion = calculate_margin_cushion(metrics.equity_to_margin_ratio);
    metrics.cash_available = portfolio_value - metrics.total_posted_margin;

    INFO("Margin calculation complete: " +
         std::to_string(metrics.active_positions) + " active positions, " +
         "gross_notional=$" + std::to_string(metrics.gross_notional) + ", " +
         "posted_margin=$" + std::to_string(metrics.total_posted_margin) + ", " +
         "leverage=" + std::to_string(metrics.gross_leverage) + "x");

    return Result<MarginMetrics>(metrics);
}

Result<std::pair<double, double>> MarginManager::calculate_position_margin(
    const std::string& symbol,
    double quantity,
    double market_price) {

    // Get instrument from registry
    auto instrument_result = get_instrument_safe(symbol);
    if (instrument_result.is_error()) {
        return make_error<std::pair<double, double>>(
            ErrorCode::INVALID_DATA,
            "Failed to get instrument for " + symbol + ": " +
            std::string(instrument_result.error()->what()));
    }

    auto instrument = instrument_result.value();

    // Extract margin requirements
    auto [initial_margin, maintenance_margin] = extract_margin_requirements(instrument, quantity);

    return Result<std::pair<double, double>>(std::make_pair(initial_margin, maintenance_margin));
}

Result<double> MarginManager::calculate_position_notional(
    const std::string& symbol,
    double quantity,
    double market_price) {

    // Get instrument from registry
    auto instrument_result = get_instrument_safe(symbol);
    if (instrument_result.is_error()) {
        return make_error<double>(
            ErrorCode::INVALID_DATA,
            "Failed to get instrument for " + symbol + ": " +
            std::string(instrument_result.error()->what()));
    }

    auto instrument = instrument_result.value();

    // Get contract multiplier
    double multiplier = instrument->get_multiplier();
    if (multiplier <= 0) {
        return make_error<double>(
            ErrorCode::INVALID_DATA,
            "Invalid multiplier " + std::to_string(multiplier) + " for " + symbol);
    }

    // Calculate notional with proper contract multiplier
    double signed_notional = quantity * market_price * multiplier;

    return Result<double>(signed_notional);
}

void MarginManager::print_margin_summary(
    const MarginMetrics& metrics,
    const std::unordered_map<std::string, Position>& positions,
    const std::unordered_map<std::string, double>& market_prices) const {

    std::cout << "\n=== Margin Summary ===" << std::endl;
    std::cout << std::setw(10) << "Symbol" << " | "
              << std::setw(10) << "Quantity" << " | "
              << std::setw(10) << "Price" << " | "
              << std::setw(12) << "Notional" << " | "
              << std::setw(10) << "P&L" << std::endl;
    std::cout << std::string(60, '-') << std::endl;

    // Print each position
    for (const auto& [symbol, position] : positions) {
        if (std::abs(position.quantity.as_double()) < 1e-6) {
            continue;
        }

        double market_price = position.average_price.as_double();
        auto price_it = market_prices.find(symbol);
        if (price_it != market_prices.end()) {
            market_price = price_it->second;
        }

        auto notional_it = metrics.symbol_notionals.find(symbol);
        double notional = (notional_it != metrics.symbol_notionals.end()) ? notional_it->second : 0.0;

        std::cout << std::setw(10) << symbol << " | "
                  << std::setw(10) << std::fixed << std::setprecision(2)
                  << position.quantity.as_double() << " | "
                  << std::setw(10) << std::fixed << std::setprecision(2)
                  << market_price << " | "
                  << std::setw(12) << std::fixed << std::setprecision(2)
                  << notional << " | "
                  << std::setw(10) << std::fixed << std::setprecision(2)
                  << position.unrealized_pnl.as_double() << std::endl;
    }

    std::cout << std::endl;
    std::cout << "Active Positions: " << metrics.active_positions << std::endl;
    std::cout << "Gross Notional: $" << std::fixed << std::setprecision(2)
              << metrics.gross_notional << std::endl;
    std::cout << "Net Notional: $" << std::fixed << std::setprecision(2)
              << metrics.net_notional << std::endl;
    std::cout << "Posted Margin: $" << std::fixed << std::setprecision(2)
              << metrics.total_posted_margin << std::endl;
    std::cout << "Gross Leverage: " << std::fixed << std::setprecision(2)
              << metrics.gross_leverage << "x" << std::endl;
    std::cout << "Equity-to-Margin: " << std::fixed << std::setprecision(2)
              << metrics.equity_to_margin_ratio << std::endl;
}

Result<void> MarginManager::validate_margins(const MarginMetrics& metrics) const {
    // Check if posted margin is valid for active positions
    if (metrics.active_positions > 0 && metrics.total_posted_margin <= 0.0) {
        return make_error<void>(
            ErrorCode::INVALID_DATA,
            "Computed posted margin is non-positive while positions are active");
    }

    // Warn about low equity-to-margin ratio
    if (metrics.equity_to_margin_ratio <= 1.0 && metrics.active_positions > 0) {
        WARN("Equity-to-Margin Ratio is <= 1.0; verify margins");
    }

    return Result<void>();
}

double MarginManager::calculate_gross_leverage(
    double gross_notional,
    double portfolio_value) {

    if (portfolio_value <= 0) {
        return 0.0;
    }
    return gross_notional / portfolio_value;
}

double MarginManager::calculate_equity_to_margin_ratio(
    double gross_notional,
    double posted_margin) {

    if (posted_margin <= 0) {
        return 0.0;
    }
    return gross_notional / posted_margin;
}

double MarginManager::calculate_margin_cushion(double equity_to_margin_ratio) {
    return (equity_to_margin_ratio - 1.0) * 100.0;
}

std::string MarginManager::normalize_symbol_for_lookup(const std::string& symbol) {
    std::string lookup_sym = symbol;

    // Remove .v. suffix
    auto dotpos = lookup_sym.find(".v.");
    if (dotpos != std::string::npos) {
        lookup_sym = lookup_sym.substr(0, dotpos);
    }

    // Remove .c. suffix
    dotpos = lookup_sym.find(".c.");
    if (dotpos != std::string::npos) {
        lookup_sym = lookup_sym.substr(0, dotpos);
    }

    return lookup_sym;
}

Result<std::shared_ptr<Instrument>> MarginManager::get_instrument_safe(const std::string& symbol) {
    // Normalize symbol for lookup
    std::string lookup_sym = normalize_symbol_for_lookup(symbol);

    try {
        auto instrument_ptr = registry_.get_instrument(lookup_sym);
        if (!instrument_ptr) {
            // Log available instruments for debugging
            ERROR("Instrument " + lookup_sym + " not found in registry!");
            ERROR("Available instruments:");
            auto all_instruments = registry_.get_all_instruments();
            for (const auto& inst : all_instruments) {
                ERROR("  - " + inst.first);
            }

            return make_error<std::shared_ptr<Instrument>>(
                ErrorCode::INVALID_DATA,
                "Instrument " + lookup_sym + " not found in registry");
        }

        return Result<std::shared_ptr<Instrument>>(instrument_ptr);

    } catch (const std::exception& e) {
        return make_error<std::shared_ptr<Instrument>>(
            ErrorCode::INVALID_DATA,
            "Failed to get instrument for " + symbol + ": " + std::string(e.what()));
    }
}

std::pair<double, double> MarginManager::extract_margin_requirements(
    const std::shared_ptr<Instrument>& instrument,
    double quantity) const {

    double contracts_abs = std::abs(quantity);

    // Get initial margin
    double initial_margin_per_contract = instrument->get_margin_requirement();
    if (initial_margin_per_contract <= 0) {
        ERROR("Invalid initial margin " + std::to_string(initial_margin_per_contract) +
              " for " + instrument->get_symbol());
        throw std::runtime_error("Invalid initial margin for: " + instrument->get_symbol());
    }
    double total_initial_margin = contracts_abs * initial_margin_per_contract;

    // Try to get maintenance margin if available (for futures)
    double maintenance_margin_per_contract = initial_margin_per_contract;
    if (auto futures_ptr = std::dynamic_pointer_cast<FuturesInstrument>(instrument)) {
        maintenance_margin_per_contract = futures_ptr->get_maintenance_margin();
        if (maintenance_margin_per_contract <= 0) {
            ERROR("Invalid maintenance margin " +
                  std::to_string(maintenance_margin_per_contract) +
                  " for " + instrument->get_symbol());
            // Fall back to initial margin
            maintenance_margin_per_contract = initial_margin_per_contract;
        }
    }
    double total_maintenance_margin = contracts_abs * maintenance_margin_per_contract;

    return std::make_pair(total_initial_margin, total_maintenance_margin);
}

} // namespace trade_ngin