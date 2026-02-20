#pragma once

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include <unordered_map>
#include <string>
#include <memory>

namespace trade_ngin {

/**
 * MarginManager - Handles margin calculations and risk metrics
 *
 * This class encapsulates the logic for:
 * - Calculating posted margin requirements
 * - Computing maintenance margins
 * - Calculating leverage metrics
 * - Computing notional values
 *
 * Extracted from live_trend.cpp lines 915-1030 as part of Phase 3 refactoring
 */
class MarginManager {
private:
    InstrumentRegistry& registry_;

public:
    /**
     * Constructor
     * @param registry Reference to instrument registry for margin requirements
     */
    explicit MarginManager(InstrumentRegistry& registry)
        : registry_(registry) {}

    /**
     * Structure containing all margin-related metrics
     */
    struct MarginMetrics {
        double total_posted_margin = 0.0;        // Total initial margin posted
        double maintenance_requirement = 0.0;     // Total maintenance margin required
        double gross_notional = 0.0;             // Sum of absolute position notionals
        double net_notional = 0.0;               // Net position notional (can be negative)
        double equity_to_margin_ratio = 0.0;     // Gross notional / posted margin
        double margin_cushion = 0.0;              // (equity_to_margin - 1) * 100
        double gross_leverage = 0.0;              // Gross notional / portfolio value
        double cash_available = 0.0;             // Portfolio value - posted margin
        int active_positions = 0;                // Number of non-zero positions

        // Per-symbol breakdown (optional)
        std::unordered_map<std::string, double> symbol_margins;
        std::unordered_map<std::string, double> symbol_notionals;
    };

    /**
     * Calculate all margin metrics for current positions
     *
     * @param positions Current positions map
     * @param market_prices Market prices for positions (typically T-1 close)
     * @param portfolio_value Current portfolio value
     * @return MarginMetrics structure with all calculations
     */
    Result<MarginMetrics> calculate_margin_requirements(
        const std::unordered_map<std::string, Position>& positions,
        const std::unordered_map<std::string, double>& market_prices,
        double portfolio_value);

    /**
     * Calculate margin for a single position
     *
     * @param symbol Symbol of the position
     * @param quantity Position quantity
     * @param market_price Market price for the symbol
     * @return Pair of (initial_margin, maintenance_margin)
     */
    Result<std::pair<double, double>> calculate_position_margin(
        const std::string& symbol,
        double quantity,
        double market_price);

    /**
     * Calculate notional value for a position
     *
     * @param symbol Symbol of the position
     * @param quantity Position quantity
     * @param market_price Market price for the symbol
     * @return Notional value (signed)
     */
    Result<double> calculate_position_notional(
        const std::string& symbol,
        double quantity,
        double market_price);

    /**
     * Print margin summary to console
     *
     * @param metrics Margin metrics to print
     * @param positions Current positions for detailed output
     * @param market_prices Market prices for display
     */
    void print_margin_summary(
        const MarginMetrics& metrics,
        const std::unordered_map<std::string, Position>& positions,
        const std::unordered_map<std::string, double>& market_prices) const;

    /**
     * Validate margin requirements
     *
     * @param metrics Calculated margin metrics
     * @return Result indicating if margins are valid, with error message if not
     */
    Result<void> validate_margins(const MarginMetrics& metrics) const;

    /**
     * Calculate gross leverage
     *
     * @param gross_notional Total gross notional exposure
     * @param portfolio_value Current portfolio value
     * @return Gross leverage ratio
     */
    static double calculate_gross_leverage(
        double gross_notional,
        double portfolio_value);

    /**
     * Calculate equity-to-margin ratio
     *
     * @param gross_notional Total gross notional exposure
     * @param posted_margin Total posted margin
     * @return Equity-to-margin ratio
     */
    static double calculate_equity_to_margin_ratio(
        double gross_notional,
        double posted_margin);

    /**
     * Calculate margin cushion percentage
     *
     * @param equity_to_margin_ratio Current equity-to-margin ratio
     * @return Margin cushion as percentage
     */
    static double calculate_margin_cushion(double equity_to_margin_ratio);

private:
    /**
     * Normalize symbol for instrument lookup
     * Removes variant suffixes (.v.0, .c.0) for registry lookup
     *
     * @param symbol Original symbol with potential suffix
     * @return Normalized symbol for lookup
     */
    static std::string normalize_symbol_for_lookup(const std::string& symbol);

    /**
     * Get instrument from registry with error handling
     *
     * @param symbol Symbol to lookup (will be normalized internally)
     * @return Instrument pointer or error
     */
    Result<std::shared_ptr<Instrument>> get_instrument_safe(const std::string& symbol);

    /**
     * Extract margin requirements from instrument
     *
     * @param instrument Instrument pointer
     * @param quantity Position quantity
     * @return Pair of (initial_margin, maintenance_margin)
     */
    std::pair<double, double> extract_margin_requirements(
        const std::shared_ptr<Instrument>& instrument,
        double quantity) const;
};

} // namespace trade_ngin