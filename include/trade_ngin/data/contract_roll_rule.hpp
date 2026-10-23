// include/trade_ngin/data/contract_roll_rule.hpp
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/data/contract_bars.hpp"
#include "trade_ngin/data/roll_calendar.hpp"
#include "trade_ngin/data/roll_cycle.hpp"

namespace trade_ngin::contract_rolls {

/**
 * @brief The per-contract roll rule: which contract the futures book holds on each date of the
 *        vendor's grid, and when it rolls (HD 2026-10-23; ROLLS_STEP1_PLAN section 11, which replaces
 *        sections 3, 5c and 10b point 4 where they differ).
 *
 * HD's rule, as ruled:
 *
 *   For the held contract:
 *   - Deadline D*: its first notice day, or its last trading day where that comes first or where it
 *     has no notice day.
 *   - Window: the last five trading days before D*.
 *   - Last actionable bar: the last trading-day bar B before D* whose next calendar day is still
 *     before D*. The live runners run seven days a week and the run dated R trades the bar of R-1
 *     (a Saturday run trades Friday's bar at Friday's settle), so this is the last bar whose trade
 *     happens before D*.
 *   - The roll happens on the first of:
 *     (a) the vendor's confirmed forward switch (a contract expiring later than the held one, kept
 *         by the next bar: today's confirmation rule), on the vendor's date;
 *     (b) the first trading-day bar inside the window where the target contract's volume is greater
 *         than the held contract's;
 *     (c) the last actionable bar.
 *   - Target: the front month of the root's cycle: the next contract after the held one, in expiry
 *     order, whose month is in the cycle and which has a bar on the roll bar. If it has no bar, the
 *     next cycle contract that does, with a WARN. A vendor switch into a contract outside the cycle
 *     gives the timing only; the target is still the cycle front (count these).
 *   - Never back, strays ignored: a vendor switch to an earlier-expiring contract, or a one-bar
 *     stray, is ignored and the held contract's own bar is read.
 *   - Rolls fall only on trading-day bars. A vendor switch on a Sunday or a Globex holiday session
 *     rolls on the next trading-day bar where both contracts print.
 *   - Series start: on the first date, hold the vendor's contract if it is in the cycle and not
 *     past its last actionable bar; otherwise the cycle front.
 *
 *   Missing bars:
 *   - Class c, the held contract has no bar on a grid date: carry its last close when the vendor
 *     names another contract; when the vendor names the held contract and the table has no row,
 *     use the vendor's bar with a WARN.
 *   - Old contract with no bar on the roll bar: move to the next trading-day bar where both print,
 *     never later than the last actionable bar. If none exists, the plan's section 5c fallback
 *     with a WARN, and list every such case.
 *   - Class d: the vendor's contract by symbol and instant (contract_bars.hpp).
 *
 * Where the ruling is silent, this code decides as follows (R2 report, "Interpretations"):
 *   - The window's first day is the 5th trading day strictly before D*; the last actionable bar
 *     (LAB) is the latest trading day t with t + 1 day < D*, a calendar date (the grid may lack it).
 *   - A vendor switch is a bar whose vendor contract differs from the last KNOWN vendor contract
 *     before it, expires later than the held one, and is the vendor's contract on the next grid
 *     bar too. A switch to a contract later than the target still rolls only to the target.
 *   - On each trading-day bar with no roll pending: (a) if a vendor switch is pending, else (b),
 *     else (c) when the bar is on or after the LAB (a deadline past the LAB when the grid has no bar
 *     on it is flagged late). A pending roll executes on the first trading-day bar where the held
 *     contract has a row and a target prints; at or after the LAB with no held row it rolls with
 *     the fallback (the new contract's move with the gap on the previous grid date when the new
 *     contract printed there, else 0).
 *   - The target on a bar is the earliest-expiring contract printing that bar whose month is in
 *     the cycle and which expires after the held one; flagged when it is not the cycle front.
 *
 * The rule reads only the vendor's grid (dates and ids), the contract bars and the calendar: it
 * never reads a price to name a contract.
 */
enum class RollTrigger { NONE, START, VENDOR, CROSSOVER, DEADLINE, ALIAS };
/// "", "start", "vendor", "crossover", "deadline", "alias".
const char* to_string(RollTrigger trigger);

/// Where the day's bar comes from.
enum class BarSource {
    OWN,     ///< the held contract's own row
    VENDOR,  ///< class c, the vendor names the held contract and the table has no row (WARN)
    CARRY,   ///< class c, the vendor names another contract: the held contract's last close
};
/// "own", "vendor", "carry".
const char* to_string(BarSource source);

/// The plan's section 5c fallback, on a roll whose old contract has no bar up to the LAB.
enum class RollFallback {
    NONE,
    NEW_CONTRACT_MOVE,  ///< the new contract's move, the gap taken on the previous grid date
    ZERO,               ///< the new contract did not print on the previous grid date either
};
/// "", "new_move", "zero".
const char* to_string(RollFallback fallback);

/// The dates the rule reads for one contract.
struct ContractDates {
    Day last_trading_day{};                ///< the expiration column
    std::optional<Day> first_notice_day;   ///< the exchange rule (roll_calendar.hpp)
    Day deadline{};                        ///< D*
    Day window_first{};                    ///< the 5th trading day before D*
    Day last_actionable{};                 ///< the LAB
    std::optional<Day> rule_last_trading_day;  ///< the exchange rule's, for the cross-check
};

/// The contract's dates. Throws CalendarCoverageError outside the calendar.
ContractDates contract_dates(const ContractKey& key, const TradingCalendar& cal);

/// The rule's target on `day` for the held contract: the earliest-expiring contract of `root` with a
/// row that day whose month is in `cycle` and which expires after the held one; nullptr when none
/// prints. An error naming a candidate with no expiration.
Result<const Contract*> cycle_target(const ContractBook& book, const std::string& root,
                                     const std::string& cycle, const ContractKey& held, Day day);

/// One grid date of the held series.
struct HeldDay {
    Day date{};
    std::optional<ContractKey> vendor;  ///< the vendor's contract, when its id names one
    ContractKey held;                   ///< after any roll on this date
    std::string bar_id;                 ///< the id of the row read (empty when carried)
    double open{0.0};
    double high{0.0};
    double low{0.0};
    double close{0.0};
    int64_t volume{0};
    bool has_close{true};  ///< false only when carrying before the held contract's first row
    BarSource source{BarSource::OWN};

    bool roll{false};
    std::optional<ContractKey> old;   ///< roll bars: the contract left
    std::optional<double> old_close;  ///< roll bars: its close on this date (none on a fallback)
    RollTrigger trigger{RollTrigger::NONE};  ///< START on the first date, the trigger on a roll
    Day trigger_date{};                      ///< roll bars: the bar the trigger was first set on
    RollFallback fallback{RollFallback::NONE};
    std::optional<double> fallback_new_previous_close;  ///< NEW_CONTRACT_MOVE: the gap date's close
    bool deferred{false};              ///< the roll fell later than its trigger's bar
    bool target_skipped_front{false};  ///< the target is not the cycle front (WARN)
    bool vendor_off_cycle{false};      ///< a vendor switch into a month outside the cycle timed it
    bool late{false};                  ///< a deadline roll after the LAB (no grid bar on the LAB)
};

/// Counts and messages of one series, for the report and the measures.
struct RuleReport {
    std::map<std::string, int> rolls_by_trigger;
    int rolls{0};
    int deferred{0};
    int fallbacks{0};
    int target_skipped_front{0};
    int vendor_off_cycle{0};
    int late{0};
    int carry_days{0};
    int vendor_bar_days{0};
    int unknown_vendor_days{0};
    int no_target_bars{0};      ///< trading-day bars a pending roll found no target printing
    int ltd_mismatches{0};      ///< contracts whose expiration differs from the exchange rule
    std::vector<std::string> fallback_cases;  ///< every fallback roll, listed
    std::vector<std::string> warnings;
};

struct HeldSeries {
    std::string root;
    std::vector<HeldDay> days;
    RuleReport report;
};

/**
 * @brief The micros' full-size contracts before the vendor's 2026-02-22 change (ROLLS_STEP1_PLAN 10b
 *        point 5): until then MES.v.0, MNQ.v.0, MYM.v.0 and M2K.v.0 are the ES, NQ, YM and RTY
 *        contracts (same index price and expiries, another multiplier and volume). Before `change`
 *        the series reads the full-size root's contracts and the vendor's id names one of them; from
 *        it, the micro's own. The change is one roll (trigger ALIAS) between the two contracts of the
 *        same expiry, on the first trading-day bar on or after `change` where both print.
 */
struct MicroAlias {
    std::string full_root;  ///< "ES"
    Day change{};           ///< the first date the vendor names the micro's own contract
};

/// The micro alias of `root` ("MES" -> ES, 2026-02-22), or nullopt.
std::optional<MicroAlias> micro_alias(const std::string& root);

/// The inputs of one root's series.
struct RuleInputs {
    std::string root;
    std::vector<VendorBar> grid;  ///< the vendor's grid over the range, date order
    const ContractBook* book{nullptr};
    const TradingCalendar* cal{nullptr};
    std::string cycle;  ///< the root's cycle (roll_cycle_table)
    std::optional<MicroAlias> alias;  ///< the micros only
};

/**
 * @brief Applies the rule over the grid. An error, naming the cause, for: a contract the rule needs
 *        with no expiration; a date outside the calendar; no cycle contract printing on the first
 *        date.
 */
Result<HeldSeries> apply_roll_rule(const RuleInputs& in);

}  // namespace trade_ngin::contract_rolls
