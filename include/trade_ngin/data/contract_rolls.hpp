// include/trade_ngin/data/contract_rolls.hpp
#pragma once

#include <memory>
#include <optional>
#include <string>

#include <pqxx/pqxx>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/data/contract_bars.hpp"
#include "trade_ngin/data/contract_roll_rule.hpp"
#include "trade_ngin/data/roll_calendar.hpp"
#include "trade_ngin/data/roll_cycle.hpp"

namespace trade_ngin::contract_rolls {

/**
 * @brief The one call a caller makes for the per-contract roll series of a root
 *        (ROLLS_STEP1_PLAN step 2; HD's rule in contract_roll_rule.hpp).
 *
 * NOT WIRED: nothing in the engine calls this yet. Step 3 (the bar source) and step 4 (the email's
 * "Currently holding X, switch to Y by <date>") will, behind portfolio.json's per_contract_rolls
 * switch (AppConfig::per_contract_rolls, off by default). Today only the unit tests and the
 * contract_rolls_dump tool call it.
 *
 * Per grid date the series gives: the held contract (identity and the id string of the row read),
 * its bar, the roll flag, the old contract's close on a roll bar, and the trigger (start, vendor,
 * crossover, deadline, or alias for the micros' change). For any date, outlook() gives the held
 * contract's D*, its window's first day, its last actionable bar and the target contract.
 */
struct RollOutlook {
    Day date{};
    ContractKey held;
    ContractDates dates;                  ///< D*, the window's first day, the LAB
    std::optional<ContractKey> target;    ///< the contract the rule would roll into on `date`
    bool target_is_cycle_front{false};    ///< false when the cycle front prints no bar that day
    std::string cycle_front_symbol;       ///< "GCZ6": the next cycle month after the held one
};

class ContractRollSeries {
public:
    /// An empty series (Result needs a default); its calendar covers nothing.
    ContractRollSeries()
        : cal_([](const std::string&) { return false; }, [](const std::string&) { return false; }) {}
    ContractRollSeries(HeldSeries series, std::shared_ptr<const ContractBook> book,
                       std::string cycle, std::optional<MicroAlias> alias, TradingCalendar cal);

    const HeldSeries& series() const { return series_; }
    const std::vector<HeldDay>& days() const { return series_.days; }
    const RuleReport& report() const { return series_.report; }

    /// The held contract's dates and target on `date` (the latest grid date on or before it).
    /// An error before the series' first date, or outside the calendar.
    Result<RollOutlook> outlook(Day date) const;

private:
    HeldSeries series_;
    std::shared_ptr<const ContractBook> book_;
    std::string cycle_;
    std::optional<MicroAlias> alias_;
    TradingCalendar cal_;
};

/// The first grid date of `root`'s vendor series on which the contracts the series reads that day
/// have a row (for a micro before 2026-02-22, its full-size root's). Read only.
Result<Day> series_start(pqxx::transaction_base& txn, const std::string& root);

/**
 * @brief Reads the vendor's grid and the contract bars of `root` over [start, end] and applies the
 *        rule. Read only.
 *
 * The trading days are the root's own exchange calendar (`calendars.for_root(root)`). The rule's
 * warnings dated before `log_from` are kept in the report but not logged (RuleInputs::log_from).
 *
 * Errors: a root with no cycle or no futures calendar; a contract the rule needs with no expiration (named); a date
 * outside the calendar; for a micro whose range starts before 2026-02-22, no full-size contract
 * bars before the change ("waiting on data": ES, NQ, YM and RTY are not loaded yet); a database
 * error.
 */
Result<ContractRollSeries> build_held_series(pqxx::transaction_base& txn, const std::string& root,
                                             Day start, Day end, const FuturesCalendar& calendars,
                                             Day log_from = Day{});

}  // namespace trade_ngin::contract_rolls
