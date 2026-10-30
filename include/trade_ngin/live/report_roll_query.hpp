// include/trade_ngin/live/report_roll_query.hpp
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/report_roll_view.hpp"
#include "trade_ngin/data/roll_series.hpp"

namespace trade_ngin {

/**
 * @brief What a futures live runner asks the report's roll view for, with
 *        per_contract_rolls on (PostgresDatabase::report_roll_view).
 *
 *   - date: the settled day (the runner's T-1);
 *   - held_symbols: every symbol with a non-zero quantity in the book or in any sleeve;
 *   - held_ids: per symbol in `roll_status` (the status of its last consumed bar), the contract the book
 *     holds after that bar, so the old contract until its roll legs are booked;
 *   - rolls: every ROLL closing leg of the run (EXEC_<symbol>_<YYYYMMDD>_RC) with the contract it closes,
 *     the contract its opening leg (_RO) opens and the date in its id (the confirming bar's); a leg with
 *     no split is asked for too and gets no line (with the switch on every closing leg has one).
 */
contract_rolls::ReportRollQuery report_roll_query(
    const std::string& settled_date, const std::unordered_map<std::string, Position>& book,
    const std::unordered_map<std::string, std::unordered_map<std::string, Position>>& sleeves,
    const std::unordered_map<std::string, std::vector<ExecutionReport>>& sleeve_executions,
    const std::unordered_map<std::string, roll_series::RollTracker::Status>& roll_status);

/// The registry's tick size of a futures symbol ("SI.v.0"), 0 when it is not known (the roll line's
/// price difference then prints with four decimals).
double report_tick_size(const std::string& symbol);

}  // namespace trade_ngin
