// Conservative live portfolio.
//
// The daily pipeline lives in live_portfolio_runner.cpp, shared with
// live_portfolio. This file now only says which portfolio to run.
//
// The runner was derived from this file's previous contents, so this portfolio's
// behaviour is unchanged by the deduplication.

#include "live_portfolio_runner.hpp"

int main(int argc, char* argv[]) {
    trade_ngin::LivePortfolioConfig cfg;
    cfg.log_prefix = "live_trend_conservative";
    cfg.config_name = "conservative";

    // Left unset deliberately: this portfolio relied on the struct default for
    // the slow-strategy fallback branch rather than pinning it.
    // cfg.slow_max_symbol_concentration stays empty.

    return trade_ngin::run_live_portfolio(cfg, argc, argv);
}
