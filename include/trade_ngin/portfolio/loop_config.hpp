#pragma once

// LOOP_SPEC section 7.7: the one pass's constants, from a futures book's loaded files into its
// PortfolioConfig. The runners call this after ConfigLoader::require_loop_keys has passed, so
// every value is one the files carry: the deferral band and B_sigma's floor from defaults.json's
// optimization block, the per-name cap and the trim cap from risk.json's carver module (the
// module that carries the overlay's limits). The search's cost multiplier and the floor on its
// pass cap are opt_config's, read by the loader.

#include <variant>

#include "trade_ngin/core/config_loader.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"

namespace trade_ngin {

inline void apply_loop_config(const AppConfig& app, PortfolioConfig& portfolio) {
    portfolio.sign_close_band = app.sign_close_band;
    portfolio.b_sigma_floor = app.b_sigma_floor;
    portfolio.sizing_mode = app.sizing_mode;
    portfolio.equity_slow_symbols = app.equity_slow_rule.symbols;
    portfolio.equity_slow_pairs = app.equity_slow_rule.pairs;
    for (const auto& module : app.risk_schema.portfolio) {
        const auto* carver = std::get_if<CarverModuleConfig>(&module.params);
        if (carver == nullptr || !carver->overlay_limits()) continue;
        portfolio.per_name_cap = carver->per_name_cap;
        portfolio.trim_max = carver->trim_max;
    }
}

/// The gross leverage limit of a futures book, L_max (the max_gross_leverage of the module that
/// carries the overlay's limits; 0 when the book has none). The runners hand it to each sleeve as
/// StrategyConfig::max_leverage, which a strategy's start-up validation requires to be positive
/// and its own leverage warning compares against: the retired risk.json max_leverage used to feed
/// both.
inline double loop_gross_leverage_limit(const AppConfig& app) {
    for (const auto& module : app.risk_schema.portfolio) {
        const auto* carver = std::get_if<CarverModuleConfig>(&module.params);
        if (carver != nullptr && carver->overlay_limits()) return carver->max_gross_leverage;
    }
    return 0.0;
}

}  // namespace trade_ngin
