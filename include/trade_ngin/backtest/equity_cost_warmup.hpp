// include/trade_ngin/backtest/equity_cost_warmup.hpp
//
// The equity backtest's cost warm-up (ledger H-13; HD 2026-09-21, the root fix).
//
// bt_equity_mr registers a liquidity-tiered equity cost config per symbol before the run, from the
// ADV of a short bar window, on BOTH cost managers that price a backtest fill (the execution
// manager's and the PortfolioManager's own, E2-C9). Two defects lived in how that window was read:
//
//  * It was the 30 days AFTER start_date, [start, start + 30 d]: bars the backtest had not reached
//    set the tier every later fill is priced with (a look-ahead).
//  * It was loaded with the MarketDataBus ON. PostgresDatabase::get_market_data publishes every row
//    as a BAR event and the PortfolioManager, subscribed since construction, runs a full
//    process_market_data per event: the strategies received the window's ~21 future bars before
//    the run (the mean-reversion sleeve entered the run with its price deque already filled, and
//    two phantom executions were stored), and the PM's own date-keyed history kept those closes
//    on the window's own adjustment basis until the run re-fed the same dates on the run's basis
//    (189 PM_HISTORY_REPEATED_DATE WARNs, a mixed-basis series for 21 days).
//
// Now the window is the 30 calendar days BEFORE start_date, ending strictly before it
// ([start - 30 x 24 h, start - 1 s], both ends inclusive in the loader's BETWEEN, so it shares no
// instant with the run's own [start, end] load), and it is loaded with bus publishing disabled
// (the guard the futures runners and BacktestCoordinator::run_portfolio put around their loads),
// restored afterwards. The bars feed exactly the two cost registrations and nothing else.
//
// "30 days" stays CALENDAR days, as the window has always been counted (24 h x 30): about 21
// trading bars, of which register_equity_costs_from_bars averages the last 20.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"

namespace arrow {
class Table;
}

namespace trade_ngin {

class PortfolioManager;

namespace transaction_cost {
class TransactionCostManager;
}

namespace backtest {

// Calendar days of bars the cost warm-up reads, all of them before start_date.
inline constexpr int kEquityCostWarmupCalendarDays = 30;

struct EquityCostWarmupWindow {
    Timestamp start;  // start_date - 30 x 24 h (inclusive)
    Timestamp end;    // start_date - 1 s (inclusive): strictly before start_date
};

// The window for a backtest that starts at start_date.
EquityCostWarmupWindow equity_cost_warmup_window(const Timestamp& start_date);

// Loads the warm-up bars for [from, to] (both inclusive), in the runner:
// db->get_market_data(symbols, from, to, EQUITIES, DAILY, "ohlcv").
using EquityCostWarmupLoader = std::function<Result<std::shared_ptr<arrow::Table>>(
    const Timestamp& from, const Timestamp& to)>;

struct EquityCostWarmupSummary {
    EquityCostWarmupWindow window{};
    bool loaded = false;  // the load and the conversion both succeeded
    size_t bars = 0;
    std::unordered_map<std::string, std::vector<Bar>> bars_by_symbol;
    int registered_execution_costs = 0;
    int registered_portfolio_costs = 0;
};

// Loads the window with MarketDataBus publishing disabled (restored to its previous state after
// the load, also on an exception), then registers the tiered equity cost configs from those bars
// on `execution_costs` (the BacktestCoordinator's execution manager's) and on `portfolio` (the
// PortfolioManager's own cost manager). A failed load or conversion WARNs and registers nothing,
// as before: each symbol then falls to the untiered equity default at its first cost.
EquityCostWarmupSummary register_equity_cost_warmup(
    const std::vector<std::string>& symbols, const Timestamp& start_date,
    const EquityCostWarmupLoader& load,
    transaction_cost::TransactionCostManager& execution_costs, PortfolioManager& portfolio);

}  // namespace backtest
}  // namespace trade_ngin
