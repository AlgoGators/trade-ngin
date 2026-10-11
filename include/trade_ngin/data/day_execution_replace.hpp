// include/trade_ngin/data/day_execution_replace.hpp
#pragma once

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"

namespace trade_ngin {

/**
 * @brief The replace of a day's executions, sleeve by sleeve, on ONE open transaction (T-ROLLX-FIX
 *        commits 5 and 6; T-DATED R3, HD 2026-10-10). Per sleeve, in this order:
 *
 *          1. the sweep: every row of the sleeve and portfolio dated `day`, of every type, so an
 *             order an earlier run of the date made and this run no longer makes does not stay;
 *          2. the delete of the rows carrying this run's order ids (no calendar date: E2-F4, F-C);
 *          3. the insert of this run's rows.
 *
 *        The transaction is committed once, after every sleeve, and only when every step
 *        succeeded. A step that fails returns its error with nothing committed, so the sleeve's
 *        rows of the day are the rows that were stored before the call: a re-run whose store
 *        fails loses nothing.
 *
 *        `Txn` is the transaction as four operations (PostgresDatabase adapts pqxx::work; the unit
 *        tests use an in-memory stand-in whose store can be made to fail):
 *          void delete_day(sleeve, portfolio_id, day);
 *          void delete_order_ids(sleeve, portfolio_id, sorted unique order ids);
 *          Result<void> insert(sleeve, executions);
 *          void commit();
 */
template <class Txn>
Result<void> replace_day_executions_in(
    Txn& txn, const std::vector<std::pair<std::string, std::vector<ExecutionReport>>>& sleeves,
    const std::string& portfolio_id, const std::string& day) {
    for (const auto& [strategy_name, executions] : sleeves) {
        txn.delete_day(strategy_name, portfolio_id, day);
        std::vector<std::string> order_ids;
        order_ids.reserve(executions.size());
        for (const auto& e : executions) order_ids.push_back(e.order_id);
        std::sort(order_ids.begin(), order_ids.end());
        order_ids.erase(std::unique(order_ids.begin(), order_ids.end()), order_ids.end());
        if (!order_ids.empty()) txn.delete_order_ids(strategy_name, portfolio_id, order_ids);
        auto inserted = txn.insert(strategy_name, executions);
        if (inserted.is_error()) return inserted;
    }
    txn.commit();
    return Result<void>();
}

}  // namespace trade_ngin
