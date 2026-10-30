// include/trade_ngin/data/day_signal_replace.hpp
#pragma once

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"

namespace trade_ngin {

/**
 * @brief The sweep a store of a sleeve's signals issues before its rows: every signal
 *        row of the portfolio, the strategy id and the sleeve on the stored day that this store
 *        will NOT write, that is a row of a symbol the run does not produce, or a row of the day
 *        at another time of day. $1 portfolio_id, $2 strategy_id, $3 strategy_name, $4 the day as
 *        YYYY-MM-DD (the sessions are UTC), $5 the run's symbols, $6 the run's timestamp.
 */
inline std::string day_signal_sweep_statement(const std::string& table_name) {
    return "DELETE FROM " + table_name +
           " WHERE portfolio_id = $1 AND strategy_id = $2 AND strategy_name = $3"
           " AND \"timestamp\"::date = $4::date"
           " AND NOT (symbol = ANY($5) AND \"timestamp\" = $6::timestamptz)";
}

/**
 * @brief The replace of a sleeve's signals of a day on ONE open transaction (the model is
 *        replace_day_executions_in), so a symbol a re-run of the date no longer produces does not
 *        keep the row of the earlier run:
 *
 *          1. the sweep: the sleeve's rows (portfolio, strategy id, sleeve) of `day` that this
 *             run does not write;
 *          2. the store of this run's rows, one a symbol: an upsert on the row's key, so a row an
 *             earlier run of the date stored keeps its id and its created_at (the first insert)
 *             and takes this run's value.
 *
 *        After the commit the sleeve's rows of the day are this run's set. The transaction is
 *        committed once, and only when every row was stored. A row that fails returns its error
 *        with nothing committed, so the day's rows are the rows that were stored before the
 *        call. A first run of a day sweeps nothing.
 *
 *        `Txn` is the transaction as three operations (PostgresDatabase adapts pqxx::work; the
 *        unit tests use an in-memory stand-in whose store can be made to fail):
 *          void sweep(strategy_id, strategy_name, portfolio_id, day, sorted symbols);
 *          Result<void> store(symbol, signal);
 *          void commit();
 */
template <class Txn>
Result<void> replace_day_signals_in(Txn& txn,
                                    const std::unordered_map<std::string, double>& signals,
                                    const std::string& strategy_id,
                                    const std::string& strategy_name,
                                    const std::string& portfolio_id, const std::string& day) {
    std::vector<std::string> symbols;
    symbols.reserve(signals.size());
    for (const auto& [symbol, signal] : signals) {
        (void)signal;
        symbols.push_back(symbol);
    }
    std::sort(symbols.begin(), symbols.end());
    txn.sweep(strategy_id, strategy_name, portfolio_id, day, symbols);
    for (const auto& [symbol, signal] : signals) {
        auto stored = txn.store(symbol, signal);
        if (stored.is_error())
            return stored;
    }
    txn.commit();
    return Result<void>();
}

}  // namespace trade_ngin
