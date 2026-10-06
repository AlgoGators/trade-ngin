// Base live portfolio.
//
// The daily pipeline lives in live_portfolio_runner.cpp, shared with
// live_portfolio_conservative. This file now only says which portfolio to run.
//
// It used to be a 3,253-line near-copy of the conservative app -- 98% identical,
// which meant every fix had to be made twice. Some were not: the MarketDataBus
// duplicate-processing guard only ever landed in the conservative copy, so this
// portfolio ran without it. Sharing the implementation fixes that.

#include "live_portfolio_runner.hpp"

int main(int argc, char* argv[]) {
    trade_ngin::LivePortfolioConfig cfg;
    cfg.log_prefix = "live_trend";
    cfg.config_name = "base";

    // The base portfolio pinned this in the slow-strategy fallback branch, where
    // the conservative one relied on the struct default. Preserved verbatim so
    // deduplication does not change what this portfolio does.
    cfg.slow_max_symbol_concentration = 0.15;

    return trade_ngin::run_live_portfolio(cfg, argc, argv);
}
