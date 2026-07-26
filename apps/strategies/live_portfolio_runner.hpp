// Shared entry point behind both live portfolio executables.
//
// live_portfolio.cpp and live_portfolio_conservative.cpp were 98% identical --
// 121 differing lines out of roughly 6,550 -- so every change had to be made
// twice. In practice some were not: the MarketDataBus duplicate-processing
// guard existed only in the conservative copy, and the base portfolio quietly
// went without it.
//
// The whole daily pipeline now lives in one place, parameterised by the handful
// of values that genuinely differ between the two portfolios.
//
// This also creates the seam the independent "system" benchmark needs: with the
// pipeline callable rather than inlined in main(), it can be invoked twice from
// two different prior-position states -- once for the real book, once for the
// untouched counterfactual.

#pragma once

#include <optional>
#include <string>

namespace trade_ngin {

/// The values that actually differ between the base and conservative portfolios.
struct LivePortfolioConfig {
    /// Log filename prefix, e.g. "live_trend" or "live_trend_conservative".
    std::string log_prefix;

    /// Portfolio config to load from ./config/portfolios/, e.g. "base" or "conservative".
    std::string config_name;

    /// Concentration cap applied in the slow-strategy hardcoded-defaults branch.
    ///
    /// The base portfolio pinned this to 0.15 there; the conservative one left the
    /// struct default in place. Kept optional so neither portfolio's behaviour
    /// changes as a result of the deduplication.
    std::optional<double> slow_max_symbol_concentration;
};

/// Run one full daily live-portfolio cycle. Returns a process exit code.
int run_live_portfolio(const LivePortfolioConfig& portfolio_cfg, int argc, char* argv[]);

}  // namespace trade_ngin
