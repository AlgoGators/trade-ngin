#pragma once

#include <cmath>

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
 * @brief Class-1-encoded event that is really a SPINOFF (E2-F31).
 *
 * A spinoff is not a price restatement of one symbol. The parent distributes shares of a
 * NEW company: the parent's price series steps down by the value distributed, the holder
 * keeps every parent share, and receives `child_ratio` child shares per parent share plus
 * cash in lieu of the fraction. Nothing in the per-bar columns says any of that -- Tiingo
 * encodes the step as `split_factor` (FTV 2025-06-30: 1.327) or as `div_cash` (MMM
 * 2024-04-01: 17.3875) or as nothing at all (LEN 2025-02-07), so the applier reads it as a
 * split and mints 32.7 phantom parent shares, or as a dividend and books income that never
 * arrived. The CHILD, which is the whole point of the event, is never created.
 *
 * The ratio is not inferred from the magnitude of the step -- that is exactly the provenance
 * rule E2-F9 exists to enforce. It comes from the `spinoff` row in
 * equities_data.corporate_action, whose `value` IS the child ratio (verified 5/5:
 * GE/GEV 0.25, WDC/SNDK 0.33333, FTV/RAL 0.33333, LEN/MRP 0.5, MMM/SOLV 0.25).
 */
/**
 * @brief The class-1 columns one spinoff ex-date bar carries, and the factor they amount to.
 *
 * `get_per_bar_corporate_actions` emits a bar's `split_factor` and its `div_cash` as TWO
 * rows (postgres_database.cpp:2599-2626), and both carry the same (ticker, ex_date). Seven
 * real bars in this database carry both -- HLT 2017-01-04, K 2023-10-02, MET 2017-08-07,
 * LDOS 2013-09-30, RTX 2020-04-03, ABT 2004-05-03, BX 2015-10-01 -- and every one of them is
 * a spinoff ex-date. Matching each row against the terms key separately routes the SAME
 * distribution twice: the child is delivered twice and its realized P&L booked twice
 * (E2-F47).
 *
 * The bar is one event. This collects its columns so it can be routed once, and computes the
 * factor the parent's ADJUSTED price series actually took across it -- the product of what
 * the class-1 path would have applied row by row, `split_factor x (1 + div_cash/close)`.
 *
 * That product is not a derivation, it is a measurement. Taking
 * `(adjusted_close/close)` on the ex-date bar over the same ratio on the bar before it gives
 * the step the vendor's own adjusted series took, and it equals this product on all eight
 * real cases (the seven above plus DD 2019-06-03, split-only):
 *
 *   ABT  measured 1.142569  product 1.142509      RTX  measured 2.0116284 product 2.0116303
 *   BX   measured 1.0379078 product 1.0379642     LDOS measured 0.3672580 product 0.3672829
 *   K    measured 1.1395329 product 1.1394486     HLT  measured 0.4739751 product 0.4740230
 *   MET  measured 1.2576932 product 1.2577127     DD   measured 0.3332989 product 0.3333333
 *
 * (The residual is close-column precision: the vendor rounds `adjusted_close` to 10 digits.)
 */
struct SpinoffBarColumns {
    bool has_split{false};
    double split_factor{1.0};      ///< the bar's `split_factor` column
    bool has_dividend{false};
    double dividend_cash{0.0};     ///< the bar's `div_cash` column, raw dollars
    /** The close the dividend factor divides by: the ex-date close, or the last one before
     *  it. The SAME denominator the class-1 applier uses, so basis and marks stay in frame. */
    double close_at_ex_date{0.0};

    /** `1 + d/c`, or exactly 1.0 when there is no dividend row or no usable close. */
    double dividend_factor() const {
        if (!has_dividend) return 1.0;
        if (!(close_at_ex_date > 0.0) || !std::isfinite(close_at_ex_date)) return 1.0;
        if (!(dividend_cash > 0.0) || !std::isfinite(dividend_cash)) return 1.0;
        return 1.0 + dividend_cash / close_at_ex_date;
    }

    /** `split_factor`, or exactly 1.0 when there is no split row or the value is unusable. */
    double split_step() const {
        if (!has_split) return 1.0;
        if (!(split_factor > 0.0) || !std::isfinite(split_factor)) return 1.0;
        return split_factor;
    }

    /** The whole step the parent's price series took across this bar. */
    double total_factor() const { return split_step() * dividend_factor(); }

    /** True when the bar carries both columns -- the E2-F47 shape, worth naming in a log. */
    bool carries_both_columns() const { return has_split && has_dividend; }

    /**
     * @brief The part of the bar's step that is a REVERSE SPLIT, not the distribution.
     *
     * E2-F48. A `split_factor < 1` on a spinoff ex-date is a reverse split that happened on
     * the same day as the spinoff -- HLT's 1-for-3 on 2017-01-04, LDOS's 1-for-4 on
     * 2013-09-30, DD's 1-for-3 on 2019-06-03 -- and it is a genuine SHARE COUNT change,
     * which a distribution never is. Reading it as the spinoff's own factor makes
     * `1 - 1/F` negative, so the child is allocated a NEGATIVE cost basis, and it suppresses
     * a real split that used to apply correctly before the spinoff path existed at all.
     *
     * A `split_factor > 1` on a spinoff ex-date is NOT treated as a forward split: on all
     * five such bars in this database (ABT, BX, K, MET, RTX) the vendor is encoding part of
     * the distribution in that column and the holder's share count did not change. That is
     * verified against the adjusted series, not assumed -- see total_factor(). A genuine
     * forward split coincident with a spinoff would fall outside this rule; none exists here.
     */
    double reverse_split_factor() const {
        const double f = split_step();
        return f < 1.0 ? f : 1.0;
    }

    /** True when a coincident reverse split has to be applied as an ordinary class-1 split. */
    bool has_reverse_split() const { return reverse_split_factor() < 1.0; }

    /**
     * @brief The distribution's own factor: the bar's step with the reverse split taken out.
     *
     * This is what the parent's cost basis is divided by when the child is delivered. The
     * reverse split then divides it again through the class-1 path, and the two together
     * reproduce total_factor() exactly, which is the step the price series took.
     */
    double spinoff_factor() const { return total_factor() / reverse_split_factor(); }

    /**
     * @brief Is there a distribution left once the reverse split is accounted for?
     *
     * DD 2019-06-03 is the case that makes this necessary: split_factor 0.33333, div_cash 0,
     * and the vendor's adjusted series takes exactly the split and prices NO distribution
     * into DD at all. Routing that as a spinoff would hand CTVA a zero cost basis and book
     * its entire first close as realized gain. There is nothing to distribute from, so the
     * bar is refused as a spinoff and its split applies as the ordinary class-1 split it is.
     */
    bool routes_a_spinoff() const { return spinoff_factor() > 1.0 + 1e-9; }
};

/**
 * @brief One company distributed by a spinoff, with the terms that price it.
 *
 * E2-F49: a spinoff is not one child. RTX 2020-04-03 distributed OTIS (0.5) AND CARR (1.0);
 * HLT 2017-01-04 distributed PK (0.6) AND HGV (0.33333). Fifteen (parent, ex-date) pairs in
 * `equities_data.corporate_action` carry more than one `spinoff` row, and the routing map
 * was keyed on (ticker, date) with a scalar value, so the last row read silently overwrote
 * every earlier one: one arbitrary child was delivered and the rest were never created.
 */
struct SpinoffChildTerms {
    std::string symbol;
    double ratio{0.0};  ///< r_i -- child shares per parent share held, from `spinoff.value`
    /**
     * P_i -- the child's first REAL close on or after the ex-date. Required: it prices the
     * cash in lieu of the fractional share, prices the whole child position under
     * LIQUIDATE_AT_FIRST_CLOSE, and is the weight in the relative-FMV allocation of the
     * parent's basis. A child with no bars at all (RAL and MRP both have zero rows in
     * equities_data.ohlcv_1d) leaves this 0 and the WHOLE event is refused rather than
     * guessed -- an allocation computed over a subset of the children is not an allocation.
     */
    double first_close{0.0};
};

struct SpinoffEvent {
    std::string parent;
    std::string ex_date;             ///< YYYY-MM-DD
    /**
     * F -- the factor the PRICE SERIES restates the parent by, and therefore the factor the
     * parent's cost basis must be divided by to stay in frame with its marks. It is the
     * DISTRIBUTION's own factor: the whole step the bar took (`split_factor x
     * (1 + div_cash/close)`, SpinoffBarColumns::total_factor) with any coincident reverse
     * split divided back out, because that part is a share-count change the class-1 applier
     * owns (E2-F47, E2-F48). Strictly greater than 1 for a real distribution.
     */
    double parent_restatement_factor{1.0};
    /** Every child the (parent, ex-date) pair delivers. Never just the last one read. */
    std::vector<SpinoffChildTerms> children;
};

/**
 * @brief What to do with a child the strategy cannot trade.
 *
 * A spinoff hands you a company you never chose to own, and usually one that is not in the
 * configured universe. Holding it makes an F-4 orphan: no bars are loaded for it, so the
 * next run reports "Missing T-1 price for symbol with a non-zero position" and rolls the
 * target back forever. The default therefore sells it at the first close, which is what an
 * index-tracking mandate does and what a discretionary holder usually does within days.
 */
enum class SpinoffChildPolicy {
    LIQUIDATE_AT_FIRST_CLOSE,  ///< default: book the child, sell it at its first close
    HOLD                       ///< keep it; only safe when the child IS in the universe
};

/** @brief Parse the `spinoff_child_policy` config string. Unknown text -> the default. */
SpinoffChildPolicy spinoff_child_policy_from_string(const std::string& s);
const char* spinoff_child_policy_to_string(SpinoffChildPolicy p);

/**
 * @brief What the lifecycle handler did to one position.
 */
enum class LifecycleOutcome {
    EXITED_AT_FINAL_CLOSE,  ///< terms unavailable -> closed at the last traded price
    CONVERTED_TO_CONTRA,    ///< terms available -> rolled into the successor symbol
    RENAMED,                ///< class 2: position re-keyed to the current symbol
    SKIPPED_NO_POSITION,    ///< event for a symbol we do not hold
    SKIPPED_NO_PRICE,       ///< exit required but no final close available
    SPUN_OFF_CHILD_HELD,    ///< E2-F31: parent restated, child position created and kept
    SPUN_OFF_CHILD_SOLD,    ///< E2-F31: parent restated, child created and sold at first close
    SKIPPED_NO_CHILD_PRICE  ///< E2-F31: the child has no close, so NOTHING was applied
};

/**
 * @brief What one child of a spinoff actually received.
 *
 * The basis is a RELATIVE-FAIR-MARKET-VALUE allocation of the value that left the parent:
 *
 *     pool per parent share = B (1 - 1/F)          the complement of B/F, so nothing is lost
 *     w_i                   = r_i * P_i            child i's value per parent share
 *     basis per child share = pool * P_i / sum(w)
 *
 * With one child this is exactly `B (1 - 1/F) / r`, the formula E2-F31 shipped with, so the
 * single-child cases are unmoved. With several it is the rule a US holder's own basis
 * allocation follows, and it conserves cost basis over the parent and EVERY child:
 * `sum_i r_i * basis_i == pool`, identically.
 */
struct SpinoffChildDelivery {
    std::string symbol;
    double ratio{0.0};          ///< r_i
    double quantity{0.0};       ///< floor(q * r_i) -- whole shares delivered
    double avg_price{0.0};      ///< the allocated basis, per child share
    double fractional{0.0};     ///< q*r_i - floor(q*r_i), paid as cash in lieu
    double cash_in_lieu{0.0};   ///< fractional * first_close
    double first_close{0.0};    ///< P_i -- the price CIL and any liquidation struck at
    double realized_delta{0.0}; ///< CIL, plus the disposal under LIQUIDATE_AT_FIRST_CLOSE
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

    // ---- E2-F31 spinoff detail (empty / zero on every other outcome) ----
    double avg_price_before{0.0};    ///< parent basis before the restatement
    double avg_price_after{0.0};     ///< parent basis after: B / F
    double ratio_change{1.0};        ///< F, the parent restatement factor
    /** One entry per child delivered (E2-F49). Empty on a refusal and on every other
     *  outcome; the refusal WARNs name the children from the event, not from here. */
    std::vector<SpinoffChildDelivery> children;

    /** "PK, HGV" -- for log lines that name what the event delivered. */
    std::string children_joined() const {
        std::string out;
        for (const auto& c : children) {
            if (!out.empty()) out += ", ";
            out += c.symbol;
        }
        return out;
    }
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
     * @brief E2-F31 -- receive the child of a spinoff instead of mangling the parent.
     *
     * Pure, in place, and cross-symbol, which is why it lives here and not in the applier:
     * a spinoff changes WHAT is held.
     *
     * **The arithmetic, and why it conserves basis exactly.** Total cost basis cannot be
     * created or destroyed by a distribution, so with `q` parent shares at basis `B`:
     *
     *     parent: q unchanged,  B_parent = B / F
     *     child:  q * r shares, B_child  = B (1 - 1/F) / r
     *
     *     q*B/F  +  q*r * B(1-1/F)/r  ==  q*B                (exactly, for any F > 1, r > 0)
     *
     * `B/F` is the same restatement the adjusted price series applies to the parent, so
     * returns stay continuous across the ex-date -- the E2-F15 frame rule, honoured rather
     * than worked around. `B(1-1/F)/r` is the fair-value allocation the vendor already
     * computed for us: the fraction of value that LEFT the parent, spread over the shares
     * that received it.
     *
     * **Fractional shares.** A holder receives whole child shares and cash for the rest, so
     * the child position is `floor(q*r)` and the remainder is sold at `child_first_close`.
     * The CIL realizes `frac * (child_first_close - B_child)` -- against the child's OWN
     * basis, not the parent's, because those shares were child shares from the instant of
     * distribution. The caller books it as a `CORPACTION_<child>_<ex_date>` execution, the
     * pattern corp-action exits already use, so it is visible to a broker reconciliation.
     *
     * **A child with no close is REFUSED, not guessed.** Both real 2025 children in this
     * database (RAL, MRP) have zero rows in equities_data.ohlcv_1d. With no price there is
     * no CIL, no liquidation and no mark, and inventing one from the parent's basis would
     * fabricate a cash figure. The event is skipped whole -- the parent is left exactly as
     * it was -- and the CALLER must then also suppress the class-1 event, because letting
     * the split_factor through is the 132.7-phantom-share defect. Nothing is recorded, so
     * the next run reconsiders it once the child's bars exist.
     *
     * @param policy  what to do with the child. LIQUIDATE_AT_FIRST_CLOSE (the default)
     *                books the child and immediately sells it at `child_first_close`,
     *                realizing against `B_child`; HOLD keeps it, which strands an
     *                unpriceable position unless the child is in the universe (F-4).
     * @return one LifecycleAdjustment per event that had a held parent.
     */
    static std::vector<LifecycleAdjustment> apply_spinoffs(
        std::unordered_map<std::string, Position>& positions,
        const std::vector<SpinoffEvent>& events,
        SpinoffChildPolicy policy = SpinoffChildPolicy::LIQUIDATE_AT_FIRST_CLOSE);

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

    /**
     * @brief May a deal-terms row become a TerminationEvent for its own ticker?
     *
     * The one place the two class-3 hazards are decided together, so the runner's
     * terms loop and the tests answer the same question (E2-F26):
     *
     *  1. **Keying.** `acquisitionof`, `mergerfrom` and `spunofffrom` are the
     *     SURVIVOR's row -- the acquirer, the surviving merger party, the spinoff
     *     child. Their `ticker` is alive; `contraticker` is what died. Turning one
     *     into a TerminationEvent closes a live holding at the T-1 close and books
     *     realized P&L that never happened. 1,611 + 13 + 78 such rows sit on
     *     tickers that kept printing 30+ days later.
     *  2. **Bars contradict the row.** Same test `delisting_is_stale` applies to
     *     the timing feed, now applied to the terms feed too: a ticker still
     *     printing after the event date is not the ticker that terminated, it is a
     *     reused symbol carrying a prior issuer's row.
     *
     * A non-class-3 label is refused outright -- only a TERMINATION row builds a
     * TerminationEvent.
     *
     * @param last_bar_date newest loaded bar for the symbol; EMPTY means no bars,
     *        which is what a real termination looks like and is NOT contradiction.
     */
    static bool terms_row_terminates_its_ticker(const std::string& vendor_label,
                                                const std::string& event_date,
                                                const std::string& last_bar_date) {
        if (classify_action(vendor_label) != CorpActionClass::TERMINATION) return false;
        if (termination_keying(vendor_label) != TerminationKeying::ROW_TICKER_TERMINATES) {
            return false;
        }
        return !delisting_is_stale(event_date, last_bar_date);
    }

    static const char* outcome_to_string(LifecycleOutcome o);
};

}  // namespace trade_ngin
