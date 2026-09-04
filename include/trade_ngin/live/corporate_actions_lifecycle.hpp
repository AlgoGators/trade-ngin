#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/live/corporate_actions_classification.hpp"

namespace trade_ngin {

/**
 * @brief One historical-ticker -> current-symbol mapping (class 2).
 *
 * Sourced from equities_data.ticker_aliases. `effective_until` is the last
 * date on which the historical ticker was in use; bars on or before it belong
 * to the old symbol, bars after it to the new one.
 */
struct TickerAlias {
    std::string historical_ticker;
    std::string current_symbol;
    std::string effective_until;  // YYYY-MM-DD; empty when open-ended
    std::string note;
};

/**
 * @brief Class-3 event: the series ends, or the holding becomes something else.
 *
 * `contra_ticker` / `ratio` carry the deal terms when they are available. They
 * come only from equities_data.corporate_action, which stopped receiving
 * events on 2025-08-29 -- so for anything after that date `has_terms` is false
 * and the handler takes the documented final-close exit instead. When the feed
 * revives, the same query starts returning rows and the terms path activates
 * with no code change.
 */
struct TerminationEvent {
    std::string symbol;
    std::string event_date;    // YYYY-MM-DD (ex/effective date)
    std::string vendor_label;  // e.g. "mergerto", "delisted"
    std::string contra_ticker; // acquirer / successor symbol; empty when unknown
    double ratio{0.0};         // shares of contra per share held; 0 when unknown
    bool has_terms{false};     // true only when contra_ticker and ratio are usable
};

/**
 * @brief What the lifecycle handler did to one position.
 */
enum class LifecycleOutcome {
    EXITED_AT_FINAL_CLOSE,  ///< terms unavailable -> closed at the last traded price
    CONVERTED_TO_CONTRA,    ///< terms available -> rolled into the successor symbol
    RENAMED,                ///< class 2: position re-keyed to the current symbol
    SKIPPED_NO_POSITION,    ///< event for a symbol we do not hold
    SKIPPED_NO_PRICE        ///< exit required but no final close available
};

/**
 * @brief Auditable record of a lifecycle adjustment.
 */
struct LifecycleAdjustment {
    std::string symbol;
    std::string event_date;
    std::string vendor_label;
    CorpActionClass action_class{CorpActionClass::TERMINATION};
    LifecycleOutcome outcome{LifecycleOutcome::SKIPPED_NO_POSITION};
    double quantity_before{0.0};
    double quantity_after{0.0};   ///< 0 on exit; converted share count on rollover
    double exit_price{0.0};       ///< final close used for the exit (0 if none)
    double realized_delta{0.0};   ///< cash P&L booked by an exit
    std::string contra_ticker;    ///< populated on CONVERTED_TO_CONTRA
};

/**
 * @brief Handlers for the two classes that change WHAT you hold, not its price.
 *
 * Pure logic, no DB or file I/O: the caller sources aliases, termination
 * events and final closes, then hands them here. Mirrors the design of
 * CorporateActionsApplier (class 1) so both are directly unit-testable.
 */
class CorporateActionsLifecycle {
public:
    /**
     * @brief historical ticker -> its eras, ascending by `effective_until`.
     *
     * Each entry is (effective_until, current_symbol). ISO YYYY-MM-DD compares
     * lexicographically, so a plain sort orders the eras. An alias with no
     * `effective_until` cannot be era-bounded and is DROPPED, not applied
     * unconditionally: class 2 fails narrow, and an unbounded alias is exactly
     * the shape that re-keys a currently-trading ticker.
     */
    using RenameMap =
        std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>>;

    static RenameMap build_rename_map(const std::vector<TickerAlias>& aliases);

    /**
     * @brief The successor a ticker had at `date`: the first rename on or after it.
     *
     * A date later than every rename maps NOWHERE -- the ticker belongs to whoever
     * holds it now, not to the old company. The compare is inclusive on purpose:
     * `effective_until` conventions differ by a day between curated rows
     * (day-after) and backfilled rows (source date), so on the boundary day `<=`
     * errs toward NOT applying a backfilled rename, which is simply retried next run.
     *
     * @return (effective_until, current_symbol); both empty when nothing applies.
     */
    static std::pair<std::string, std::string> successor_at(const RenameMap& renames,
                                                            const std::string& symbol,
                                                            const std::string& date);

    /**
     * @brief The tickers `apply_renames` would move this holding onto, in hop order.
     *
     * Same chain, same era test, same as-of guard as apply_renames -- shared so the
     * universe the runner loads bars for cannot disagree with the re-keying that
     * happens later in the same run (E2-F34). Empty when no rename applies.
     */
    static std::vector<std::string> rename_chain(const RenameMap& renames,
                                                 const std::string& symbol,
                                                 const std::string& holding_start,
                                                 const std::string& as_of_date);

    /**
     * @brief Class 2 -- re-key positions held under a superseded ticker.
     *
     * A position still keyed by a historical ticker is moved to the current
     * symbol so its history stitches across the rename. Quantity and cost
     * basis are untouched (a rename is not an economic event). If both the
     * old and the new key are present, the quantities are summed and the old
     * key removed -- the two rows are the same holding.
     *
     * Aliases are a curated subset, not the full rename history: an unmapped
     * historical ticker simply stays as-is, which is the pre-existing
     * behaviour and never loses a position.
     *
     * **Renames are era-bound by position inception.** Tickers get reused: our
     * own backfill maps META -> METV (effective_until 2022-01-31), but META has
     * been Meta Platforms since, with 3,589 bars through 2026-08-28 while METV
     * has none. Applying that alias to a position opened in 2026 re-keys a live
     * holding onto a symbol with no prices. 131 historical tickers in the live
     * alias table are still actively trading, and 33 carry two or more
     * successors, so a map keyed on historical_ticker alone also picks an
     * arbitrary winner. `position_inception` supplies, per symbol, the date the
     * holding was ESTABLISHED; an alias applies only if that date falls in its
     * era (`inception <= effective_until`), resolved exactly the way the dedup
     * mirror resolves events (`corporate_actions_audit_log.cpp`). A position
     * newer than every rename for its ticker gets none -- the ticker belongs to
     * whoever holds it now.
     *
     * Class 2 fails NARROW, unlike the class-1 window: a symbol with no
     * inception entry, or an alias with no `effective_until` to bound an era
     * with, is skipped with a WARN. A skipped rename is retried next run; a
     * wrong re-key is silent corruption. Never pass class-1's fail-wide
     * bulk-start sentinel dates in here -- a sentinel two years back satisfies
     * the era test for any recent alias and reintroduces the bug.
     *
     * @param position_inception symbol -> YYYY-MM-DD the holding was established.
     */
    static std::vector<LifecycleAdjustment> apply_renames(
        std::unordered_map<std::string, Position>& positions,
        const std::vector<TickerAlias>& aliases,
        const std::string& as_of_date,
        const std::unordered_map<std::string, std::string>& position_inception);

    /**
     * @brief Class 3 -- terminate or transform holdings.
     *
     * With terms (`has_terms`): the position converts into `ratio` shares of
     * `contra_ticker`, carrying cost basis across so the roll is P&L-neutral
     * at the moment of conversion.
     *
     * Without terms: the position is closed at `final_closes[symbol]`, the
     * realized P&L booked, and a WARN emitted naming the event and the data
     * gap. This is the documented fallback and is exactly right for cash
     * deals; for stock-for-stock it approximates the post-event path.
     *
     * @param final_closes Last traded price per symbol; required for exits.
     */
    /**
     * @param feed_last_date MEASURED last row date of
     *        equities_data.corporate_action, YYYY-MM-DD. Quoted in the "no deal
     *        terms" WARNs so the operator reads what the database actually holds
     *        rather than a date compiled into the binary (E4 item 3). Empty
     *        falls back to `kCorpActionTableFrozenAfter`, which is what every
     *        caller got before the measurement existed.
     */
    static std::vector<LifecycleAdjustment> apply_terminations(
        std::unordered_map<std::string, Position>& positions,
        const std::vector<TerminationEvent>& events,
        const std::unordered_map<std::string, double>& final_closes,
        const std::string& feed_last_date = "");

    /**
     * @brief Is a `delisting_date` contradicted by the symbol's own bars?
     *
     * The same hazard the rename era test defuses, one class over: delisting_date
     * is keyed on the ticker, so a reused ticker inherits the dead company's row.
     * Acting on it exits a live position at a stale price -- the class-3 mirror of
     * re-keying META onto METV. A symbol still printing bars after its claimed
     * delisting is plainly not delisted, so the row belongs to a prior issuer and
     * the termination must be dropped.
     *
     * Both dates are ISO YYYY-MM-DD and compare lexicographically. An empty
     * `last_bar_date` (no bars loaded for the symbol) is NOT contradiction: it is
     * exactly what a real delisting looks like, so the termination stands.
     */
    static bool delisting_is_stale(const std::string& delist_date,
                                   const std::string& last_bar_date) {
        return !last_bar_date.empty() && !delist_date.empty() && last_bar_date > delist_date;
    }

    static const char* outcome_to_string(LifecycleOutcome o);
};

}  // namespace trade_ngin
