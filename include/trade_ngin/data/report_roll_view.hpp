// include/trade_ngin/data/report_roll_view.hpp
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include <pqxx/pqxx>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/report_rolls.hpp"
#include "trade_ngin/data/contract_rolls.hpp"

namespace trade_ngin::contract_rolls {

/**
 * @brief Rolls step 4 (R4b): what the daily report says about rolls, read for one run with
 *        per_contract_rolls on (report_rolls.hpp renders it).
 *
 * For each held futures symbol, its root's outlook on `date` (the settled day): the rule's series
 * built from the root's series start to `date` (build_held_series, warnings not logged), then
 * ContractRollSeries::outlook(date): the contract held, the deadline (first notice day or last trading
 * day), the window's first day, the last actionable bar (its run date is the next calendar day) and the
 * target, the cycle's front month (the contract the rule rolls into when it prints that day, else the
 * cycle front's code). A root the source leaves on the vendor's series (the micros) gets the contract
 * the vendor's id names on its latest grid bar and no window.
 *
 * For each roll the run legged, its two contracts' months, read by (root, instrument id, date) from
 * the contract table, and the root's name from the contract metadata; `tick_of` gives the contract's
 * tick for the decimals of the price difference.
 *
 * A root that cannot be read (no cycle, no calendar, a database error) is left out with a WARN: the
 * report then prints no line for it. Read only.
 */
struct ReportRollQuery {
    std::string date;                       ///< YYYY-MM-DD: the settled day
    std::vector<std::string> held_symbols;  ///< "SI.v.0"
    struct Roll {
        std::string closing_exec_id;
        std::string symbol;
        std::string outgoing_id;
        std::string incoming_id;
        std::string date;  ///< YYYY-MM-DD: the confirming bar's date (the legs' id date)
    };
    std::vector<Roll> rolls;
};

/// Where report_roll_view reads from (the database, or a test's fixtures). Each may throw; a root or a
/// roll whose read throws gets no outlook or no months, with a WARN.
struct ReportRollSources {
    /// The rule's series of `root` from its series start to `date` (warnings not logged).
    std::function<Result<ContractRollSeries>(const std::string& root, Day date)> series;
    /// The vendor's grid of `root`'s continuous series over [from, to].
    std::function<std::vector<VendorBar>(const std::string& root, Day from, Day to)> vendor_grid;
    /// The contracts of `root` with bars in the year before `day`.
    std::function<Result<ContractBook>(const std::string& root, Day day)> contracts_around;
    /// The contract metadata's names by Databento symbol.
    std::function<std::map<std::string, std::string>()> names;
};

/// The view from `sources` (pure; the database version below builds the sources).
ReportRolls report_roll_view(const ReportRollQuery& query, const ReportRollSources& sources,
                             const std::function<double(const std::string&)>& tick_of);

ReportRolls report_roll_view(pqxx::transaction_base& txn, const ReportRollQuery& query,
                             const FuturesCalendar& calendars,
                             const std::function<double(const std::string&)>& tick_of);

/// The outlook of one root from its built series (pure: the unit tests call it on fixtures).
/// `in_window` is `date` on or after the window's first day; `roll_by` the day after the last
/// actionable bar.
Result<ReportRollOutlook> report_outlook_of(const ContractRollSeries& series, const std::string& root,
                                            const std::string& cycle, Day date);

}  // namespace trade_ngin::contract_rolls
