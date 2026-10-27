// include/trade_ngin/data/per_contract_source.hpp
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <pqxx/pqxx>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/contract_rolls.hpp"
#include "trade_ngin/data/market_data_utils.hpp"

namespace arrow {
class Table;
}

namespace trade_ngin {
struct MarketDataEvent;
}

namespace trade_ngin::contract_rolls {

/**
 * @brief The futures bar source with portfolio.json's per_contract_rolls on (rolls step 3,
 *        ROLLS_STEP1_PLAN sections 2 and 5 step 3, HD's rulings of 2026-10-23 in sections 11 and
 *        11b).
 *
 * The one futures bar query underneath every loader (PostgresDatabase::get_market_data for
 * futures, and the instrument-id rows of get_futures_instrument_ids) hands out OUR series instead
 * of the vendor's `.v.0` bars: on each date of the vendor's grid, the bar of the contract HD's rule
 * holds (contract_roll_rule.hpp), its own OHLCV, `instrument_id` the held contract's id string. A
 * roll bar is the NEW contract's bar and carries the old contract's close and id in Bar::roll_from,
 * so it flows through every store untouched. The grid stays the vendor's (plan 10b point 1): the
 * same dates, the same instants, as the query returns today. Every loader, history store and
 * window above the query stays as it is.
 *
 * No look-ahead: what a run is handed for a bar depends only on that bar and the bars before it,
 * so every window that contains the bar (a live run's window, its estimator history, a backtest's
 * whole range) reads the same bar, and a backtest is a chain of dated live runs.
 *   - The rule runs from the root's series start (series_start), not from the window's start, so
 *     the held contract on a date never depends on where a caller's window begins.
 *   - It runs on the grid up to the window's last instant only: no bar after it is read.
 *   - The rule's one look-ahead is the vendor's confirmation (rule (a): the switch is a roll only
 *     if the next bar keeps the vendor's contract). On the switch bar itself a run cannot know it,
 *     so the source hands the switch bar out the same way whether or not the next bar confirms it,
 *     and whether or not the window ends on it: the HELD contract's own bar, the field naming the
 *     target (its close and id), RollFromKind::AWAITS_NEXT (HeldDay::awaits_next_bar on a confirmed
 *     switch, TentativeRoll on one the next bar did not confirm or the range ends on). Every level
 *     reader (sizing, the optimiser, the overlay, the marks) reads the held contract's close on it.
 *     The library (roll_series.hpp) reads the next consumed bar: held going into it in the target
 *     confirms the roll on the switch bar (the legs booked on it at the switch bar's two closes, its
 *     move measured from the target's close); otherwise nothing rolled and nothing changes. A run
 *     whose consumed bars end on the switch bar holds the symbol, as today.
 *   - A crossover and a deadline roll are known on the roll bar itself (RollFromKind::KNOWN), and
 *     so is a vendor switch dated on a bar where the crossover or the deadline also rolls, or
 *     confirmed by an earlier bar (a Sunday switch rolled on Monday).
 *   - Every bar after a roll, up to the next one, carries the old contract's close on that date
 *     (AFTER_ROLL), so a consumer that withheld the roll bar and any bars after it (the consumed-bar
 *     rule) books the roll on its first consumed bar, the old contract's move up to it and both
 *     legs at its closes (HD 10-23). A consumer that consumed the roll bar reads nothing from it.
 *
 * The micros (MES, MNQ, MYM, M2K) read the rule like every other root (R6, plan 10b point 5):
 * before the vendor's 2026-02-22 change their series is the full-size root's contracts (ES, NQ, YM,
 * RTY), from it the micro's own, and the change is one roll (trigger ALIAS) between the two
 * contracts of the same expiry (contract_roll_rule.hpp, MicroAlias).
 */
/// Rolls step 4, ruling 19 (HD 2026-10-23): the bar the source hands out on an unconfirmed vendor
/// switch bar (portfolio.json per_contract_switch_bar).
enum class SwitchBar {
    HELD,    ///< "held": the held contract's own bar, the field naming the target (AWAITS_NEXT)
    VENDOR,  ///< "vendor": the target's own bar, the field naming the held contract
             ///< (AWAITS_NEXT_VENDOR)
};
/// "held" or "vendor".
const char* to_string(SwitchBar switch_bar);
/// The setting's value from its name; nullopt for any other text.
std::optional<SwitchBar> switch_bar_from(const std::string& name);

class PerContractSource {
public:
    static PerContractSource& instance();

    /// Turns the source on with the futures calendar the rule counts trading days on. With
    /// `carry_quotes` every bar handed out carries the day's carry quote (attach_carry_quotes).
    void enable(FuturesCalendar calendar, SwitchBar switch_bar = SwitchBar::HELD,
                bool carry_quotes = false);
    /// Back to the vendor's series (the default).
    void disable();
    bool enabled() const;
    /// The calendar given to enable(); an empty calendar when off.
    FuturesCalendar calendar() const;
    /// Ruling 19's setting given to enable(); HELD when off.
    SwitchBar switch_bar() const;
    /// Whether the bars carry carry quotes (trade-ngin #185); false when off.
    bool carry_quotes() const;
    /// Rolls step 4 (R4a open question 4): the first bar date whose rule warnings (the fallbacks) a
    /// load logs. A live run sets its T-1, the bar it consumes for the first time (runs go on
    /// consecutive days); Day{} (the default, and after enable()) logs a load's whole window.
    void set_log_floor(Day day);
    Day log_floor() const;
    /// True the first time `message` is asked after enable() (each fallback is logged once per run,
    /// however many loads read its bar).
    bool first_log(const std::string& message);

private:
    PerContractSource() = default;
    mutable std::mutex mutex_;
    bool enabled_{false};
    std::optional<FuturesCalendar> calendar_;
    SwitchBar switch_bar_{SwitchBar::HELD};
    bool carry_quotes_{false};
    Day log_floor_{};
    std::set<std::string> logged_;
};

/**
 * @brief The runners' one call: the source on when `per_contract_rolls` is true (loading the
 *        futures calendar, FuturesCalendar::resolve_path), off otherwise; `switch_bar` is
 *        per_contract_switch_bar ("held" or "vendor", ruling 19). An error when the calendar cannot
 *        be loaded or the setting is neither; the runner refuses to run. `carry_quotes` (carry on,
 *        trade-ngin #185) makes every bar carry the day's carry quote; carry reads two contracts'
 *        own closes, so it is refused with per_contract_rolls off.
 */
Result<void> configure_per_contract_source(bool per_contract_rolls,
                                           const std::string& switch_bar = "held",
                                           bool carry_quotes = false);

/// One bar the source hands out, before the loader's Arrow conversion.
struct SourceBar {
    std::string time_text;  ///< the vendor grid bar's own `time`, as the database prints it
    Day date{};
    std::string symbol;  ///< "GC.v.0"
    double open{0.0};
    double high{0.0};
    double low{0.0};
    double close{0.0};
    double volume{0.0};
    std::string instrument_id;  ///< the held contract's id (the target's on a roll bar)
    std::optional<RollFrom> roll_from;
    std::optional<CarryQuote> carry;  ///< trade-ngin #185, with carry quotes on only
};

/**
 * @brief One root's bars on the given grid instants, from its held series (pure: the unit tests
 *        call it on fixtures).
 *
 * `series` runs from the root's series start to the window's last date at least; `window` maps
 * each grid date the caller asks for to its instant's text. Per day: a roll bar is the new
 * contract's bar with RollFrom (the old contract's close and id, KNOWN); a vendor roll awaiting the
 * next bar (HeldDay::awaits_next_bar) and a TentativeRoll are the held (old) contract's own bar
 * with RollFrom naming the target (AWAITS_NEXT); any other day the held contract's bar (a carried
 * close keeps the carried row's id), with an AFTER_ROLL RollFrom on every day after a roll, up to
 * the next, when the contract left printed that day. A fallback roll (the old contract with no bar
 * up to its last actionable bar, plan 5c) reads as the roll it stands for: RollFrom::close is the
 * previous day's close times the new contract's move (or that close, for a zero fallback), with a
 * WARN. Errors: a window date the series lacks, a day with no close. With `switch_bar` VENDOR
 * (ruling 19) a switch bar is the target's own bar instead, the field naming the held contract
 * (AWAITS_NEXT_VENDOR); every other bar is the same.
 */
Result<std::vector<SourceBar>> bars_of_series(const HeldSeries& series,
                                              const std::map<Day, std::string>& window,
                                              SwitchBar switch_bar = SwitchBar::HELD,
                                              Day log_from = Day{},
                                              const std::function<bool(const std::string&)>& log_once = {});

/**
 * @brief Carry (trade-ngin #185): each bar's carry quote (data/carry_quote.hpp), from the contracts of
 *        `book`, for the contract the book holds on the bar's date.
 *
 * The contract pair moves on the bar that confirms a roll: on a roll known on its bar the new
 * contract; on a vendor switch only the next bar confirms (HeldDay::awaits_next_bar) the old
 * contract, the new one from the next bar; on an unconfirmed switch (TentativeRoll) the held
 * contract. Which bar the source hands out on a switch bar (ruling 19) does not change the pair.
 * The neighbour is taken from the held contract's own root's roll cycle. A bar whose date the
 * series lacks, or whose held contract or neighbour has no row that day, gets no quote.
 */
void attach_carry_quotes(const HeldSeries& series, const ContractBook& book,
                         std::vector<SourceBar>& bars);

/// The roots of the symbols asked for, in the order asked: the rule serves every one of them, the
/// micros included. An error for a symbol that is not a continuous futures symbol (`<root>.v.0`).
Result<std::vector<std::string>> source_roots(const std::vector<std::string>& symbols);

/// Sorts the bars as the futures query sorts them: time, then symbol (stable).
void sort_source_bars(std::vector<SourceBar>& bars);

/// get_futures_instrument_ids' rows of the bars: one per bar with an id, ORDER BY symbol, date.
std::vector<market_data_utils::FuturesInstrumentId> instrument_id_rows(
    const std::vector<SourceBar>& bars);

/// The instant of a grid bar's `time` text, by the loader's own conversion (convert_to_arrow_table).
Timestamp source_bar_time(const std::string& time_text);

/// The bus event the loader publishes for a bar (BAR, its OHLCV), as for the vendor's bars.
MarketDataEvent source_bar_event(const SourceBar& bar);

/// The loader's table of `bars`: the futures query's eight columns plus Bar::roll_from's four
/// (roll_from_close, roll_from_id, roll_from_kind, roll_from_date; NULL where it is absent). With
/// `with_carry` (carry on) five more, Bar::carry's (carry_held_close, carry_other_close,
/// carry_months, carry_held_symbol, carry_other_symbol; NULL where it is absent); without it the
/// table is the twelve columns, unchanged.
Result<std::shared_ptr<arrow::Table>> source_bars_to_arrow(const std::vector<SourceBar>& bars,
                                                           bool with_carry = false);

/// A root's grid window from its instants' rows (the instant's text, its UTC date's text), oldest
/// first. An error for two instants on one date.
Result<std::map<Day, std::string>> grid_window(
    const std::string& root, const std::vector<std::pair<std::string, std::string>>& instants);

/// Drops the window's dates before the root's series start (no contract row yet), with a WARN
/// naming how many; returns that count.
size_t drop_before_series_start(const std::string& root, std::map<Day, std::string>& window,
                                Day start);

/// per_contract_bars' read of the grid's instants of `root` over [start_ts, end_ts]: the SELECT and its
/// parameters ($1, $2 the range; grid_rows from $3).
std::pair<std::string, pqxx::params> grid_window_query(const std::string& root,
                                                       const std::string& start_ts,
                                                       const std::string& end_ts);

/**
 * @brief Reads and builds `root`'s bars over the vendor grid's instants in [start_ts, end_ts]
 *        (the loader's predicate `time BETWEEN $1 AND $2`, timestamps as the loader formats them),
 *        the rule run from the root's series start to the window's last date. Read only. An empty
 *        vector when the grid has no bar in the window.
 */
Result<std::vector<SourceBar>> per_contract_bars(pqxx::transaction_base& txn,
                                                 const std::string& root,
                                                 const std::string& start_ts,
                                                 const std::string& end_ts,
                                                 const FuturesCalendar& calendars,
                                                 SwitchBar switch_bar = SwitchBar::HELD);

}  // namespace trade_ngin::contract_rolls
