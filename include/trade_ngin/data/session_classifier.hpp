// include/trade_ngin/data/session_classifier.hpp
#pragma once

#include <chrono>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/holiday_checker.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/market_data_utils.hpp"

namespace trade_ngin {

/**
 * @brief What one symbol's bar (or its absence) on one date is, for trading.
 *
 * HD's per-symbol session rule (2026-09-17, final; STAGE3_PLAN §27) and the classifier shipped
 * by T-CLASSIFIER_ADVERSARIAL's verdict block, with HD's 2026-09-19 rulings (the 1,000-lot
 * ceiling; the D+1 limb for a FIXED-date holiday only):
 *
 *   SESSION           a usable bar: marked at it, the signal updates, the position may change
 *                     and trade at that bar's close.
 *   JUNK              a bar that is not a price a print could locate (a corrupt or wrong-
 *                     instrument print, or a locked bar): marked at the bar's price, withheld
 *                     from signal update, sizing and orders; the position is held.
 *   NO_BAR_CLOSURE    no bar, and none was expected (a named holiday, the eve of a fixed-date
 *                     holiday, or a day this symbol does not print on): held at its last mark.
 *   NO_BAR_FEED_HOLE  no bar on a day this symbol normally prints: held at its last mark, and
 *                     an alarm (the caller logs ERROR and persists the count).
 */
enum class SessionVerdict { SESSION, JUNK, NO_BAR_CLOSURE, NO_BAR_FEED_HOLE };

/// "SESSION", "JUNK", "NO_BAR(closure)", "NO_BAR(feed hole)".
const char* to_string(SessionVerdict verdict);

/**
 * @brief HD's ruling (2026-09-25, T-7b-2 C10a): on its own day an UNCONFIRMED instrument id change
 *        is held only when its volume is below 0.10 of the symbol's norm. On the day a roll cannot
 *        be told from a one-day flip; a thin print on a new id is held (6E 2025-11-05 at 0.025 of
 *        its norm; ZN, ZF, ZT, UB 2025-09-03 below 0.01), a full-volume one trades (a real roll's
 *        first day, as the volume leader, is at or near the norm). Measured on the clone
 *        (T-7b-2_evidence/analysis/c10): btfut 2y 13 of 45 flips and 35 of 279 rolls held, 0 of
 *        the OLD book's fills delayed; holding every change held all 279 rolls and delayed 3.
 *        A ruled value like the rest of SessionClassifierConfig: nothing reads it from config.
 */
inline constexpr double kIdChangeHoldFraction = 0.10;

/**
 * @brief The rule's numbers. The defaults are the ruled values; nothing reads them from config.
 */
struct SessionClassifierConfig {
    /// JUNK when volume < junk_fraction x norm AND volume < junk_ceiling_lots (a corrupt print).
    double junk_fraction{0.01};
    /// HD 2026-09-19: the fraction limb only fires below this many lots, so an ordinary thin
    /// Sunday session of a contract with a million-lot norm is a session, not junk.
    double junk_ceiling_lots{1000.0};
    /// The absolute floor: JUNK when volume < floor_lots AND volume < floor_fraction x norm;
    /// while a symbol has no norm yet (its first bars) the floor applies on its own.
    double floor_lots{50.0};
    double floor_fraction{0.25};
    /// norm = median volume of this many trailing WEEKDAY bars of the symbol whose own verdict
    /// was SESSION, strictly before the date (weekday-only so the symbol's own weekend prints
    /// cannot drag its norm down; SESSION-only so a run of junk or stub bars cannot teach it,
    /// T-7b-1 C7b R7). Until the symbol has a SESSION weekday bar, all its weekday bars count.
    /// A median of 0 is no norm: the floor alone decides.
    int norm_window_bars{20};
    /// A missing bar is a feed hole when the symbol printed on at least one of this many
    /// preceding same-weekday dates (MAX-of-8; immune to an outage teaching the window).
    int expected_lookback_weeks{8};
    /// T-7b-2 C10a, the instrument-id continuity limb on the day (see classify_symbol_day): an
    /// UNCONFIRMED id change is held when its volume is below this fraction of the norm (or the
    /// symbol has no norm). Ruled 0.10 (kIdChangeHoldFraction). Infinity holds every unconfirmed
    /// change (a one-day flip can only be told from a roll by the next session's bar, which no
    /// engine has on the day); 0 holds none on the day (the limb then only confirms).
    double id_change_hold_fraction{kIdChangeHoldFraction};
};

/// The calendar entry for a "YYYY-MM-DD" date, if the market calendar names it.
using HolidayLookup = std::function<std::optional<HolidayInfo>(const std::string& ymd)>;

/// Adapts the runners' HolidayChecker. The checker must outlive the lookup.
inline HolidayLookup holiday_lookup(const HolidayChecker& checker) {
    return [&checker](const std::string& ymd) { return checker.get_holiday_info(ymd); };
}

/// One symbol on one date, with the inputs the verdict was taken from.
struct SymbolDayVerdict {
    std::string symbol;
    std::string date;  ///< YYYY-MM-DD, UTC (the bar key)
    SessionVerdict verdict{SessionVerdict::NO_BAR_CLOSURE};
    std::string reason;  ///< which limb decided, in words
    bool has_bar{false};
    double volume{0.0};
    double close{0.0};
    std::optional<double> norm;  ///< trailing weekday median; empty while the symbol has none
    int norm_bars{0};            ///< how many weekday bars the norm was taken over
    std::optional<double> ratio; ///< volume / norm
    std::string last_bar_date;   ///< no bar: the symbol's latest bar strictly before `date`
    int hole_age_days{-1};       ///< no bar: calendar days from last_bar_date to `date`
    /// T-7b-2 C10a: the bar's vendor instrument id (empty: none known) and the previous bar's.
    std::string instrument_id;
    std::string previous_instrument_id;
    /// T-7b-2 C10a: set when the PREVIOUS bar was an unconfirmed id change; this bar confirms it
    /// a roll (the id stayed) or a one-day flip (the id reverted), in words.
    std::string id_note;
    /// T-ROLLX-FIX: set when the JUNK verdict was decided by the instrument-id continuity limb (an
    /// unconfirmed thin id change). That bar is the change bar of LOOP_SPEC v6.1 section 2.2, held
    /// under D37 like any non-SESSION verdict but CONSUMED, never withheld (section 2.1, K-01).
    bool id_change_hold{false};

    bool is_session() const { return verdict == SessionVerdict::SESSION; }
    /**
     * @brief LOOP_SPEC v6.1 section 2.1 (K-01): a JUNK bar (a corrupt or locked print, the absolute
     *        floor) and a thin first print (the floor with no norm yet) are WITHHELD: never consumed
     *        by any consumer (the roll status, the series, the strategy feed, the PortfolioManager's
     *        history, the cost model, the risk readings) and never fed later. Every other verdict
     *        with a bar (SESSION, and the id-change hold above) is consumed. A verdict without a bar
     *        has nothing to withhold.
     */
    bool k01_withheld() const {
        return has_bar && verdict == SessionVerdict::JUNK && !id_change_hold;
    }
};

/**
 * @brief classify_symbol_day over bars already in memory.
 *
 * The bars are the de-duplicated feed: one bar per (symbol, date). The B0 loader already returns
 * that; a second bar for the same (symbol, date) given here keeps the copy the loader keeps
 * (highest volume, then the lowest close, open, high, low), so the answer never depends on order.
 *
 * The rule, in order:
 *
 *   norm(s, D) = median volume of s's trailing 20 weekday bars strictly before D whose own
 *                verdict was SESSION (T-7b-1 C7b R7); while s has no SESSION weekday bar yet,
 *                the median of its trailing 20 weekday bars (T-7a's norm, so a thin contract can
 *                start); none when s has no weekday bar before D, and none when the median is 0
 *                (a norm of 0 would disable both volume limbs).
 *
 *   bar exists for (s, D):
 *     high == low                                       -> JUNK (locked / stub; a volume test can
 *                                                          never see it)
 *     no norm (no SESSION weekday bar before D, or
 *       their median is 0) and volume < floor_lots     -> JUNK (absolute floor, first bars)
 *     volume < 0.01 x norm AND volume < 1,000 lots      -> JUNK (corrupt print)
 *     volume < 50 AND volume < 0.25 x norm              -> JUNK (absolute floor)
 *     an unconfirmed instrument id change (below)       -> JUNK (T-7b-2 C10a, HD 2026-09-24)
 *     otherwise                                         -> SESSION (thin or not: traded)
 *   no bar for (s, D):
 *     the calendar names D                              -> NO_BAR_CLOSURE
 *     D is a Saturday or Sunday, and the calendar names
 *       D+1 as a FIXED-date holiday (Christmas and New
 *       Year's Sundays; C7b R8: never a weekday D)      -> NO_BAR_CLOSURE
 *     s printed on >= 1 of the 8 preceding same-weekday
 *       dates                                           -> NO_BAR_FEED_HOLE
 *     otherwise                                         -> NO_BAR_CLOSURE (not expected)
 *
 * THE INSTRUMENT-ID CONTINUITY LIMB (T-7b-2 C10a; HD 2026-09-24 ruling 16: "a bar whose instrument
 * id differs from the previous and the next session's is JUNK"). Each bar may carry the vendor's
 * instrument_id (add_instrument_id; futures_data.ohlcv_1d_raw, the same print only). A bar b with a
 * known id whose previous bar p has a known id:
 *
 *   a ONE-DAY FLIP   id(b) != id(p) AND id(b) != id(next bar); confirmable only once the next
 *                    bar exists
 *   established(b)   the id of the newest bar before b that is not a confirmed flip
 *   an UNCONFIRMED id change   id(b) != id(p) AND id(b) != established(b) (a return from a
 *                    confirmed flip is not a change)
 *
 * The verdict of the bar dated D reads only bars dated <= D, never the next one: the live runner
 * classifies T-1 before T's bar exists, and the backtest (which already holds the next group when
 * it classifies the signal group) must see what live sees. So on its own day an id change cannot
 * be told from a roll, and an unconfirmed change is JUNK (held for one cycle, withheld from the
 * signal as any JUNK bar) when its volume is below id_change_hold_fraction x norm (ruled 0.10,
 * kIdChangeHoldFraction; a full-volume change, a roll's usual first day, trades). The next
 * session's bar confirms it: back on the established id, it was a one-day flip; on the new id, a
 * roll (id_note on that next bar's verdict says which). The norm's SESSION flags
 * take each earlier bar's verdict with that hindsight (a confirmed flip never teaches the norm; a
 * roll bar counts on its volume). A bar without an id, or whose previous bar has none, is not
 * judged by the limb.
 *
 * NOTED, NOT ACTED ON (T-CLASSIFIER_ADVERSARIAL A2, the partial-volume day): on a handful of days a
 * year the vendor delivers a real session with a truncated VOLUME field for 8-16 symbols at once
 * (2025-07-07/08: the grain complex, five currencies and three metals; 2025-11-05; 2025-09-03). The
 * closes are real prices on the path between the neighbouring closes, but the volume sits under
 * the fraction limb, so those symbols are JUNK and held for the day while the rest of the
 * universe is normal. Whether to trade at such a close is HD's ruling; this classifier holds.
 * The locked-limit limb likewise refuses both sides of a limit lock, although the side the lock
 * favours is fillable (A3); it is kept as ruled.
 */
class SessionClassifier {
public:
    using Day = std::chrono::sys_days;

    explicit SessionClassifier(SessionClassifierConfig config = {});

    /// Adds one bar (keyed on its UTC date). Bars may arrive in any order.
    void add_bar(const Bar& bar);
    void add_bars(const std::vector<Bar>& bars);

    /// T-7b-2 C10a: the vendor instrument id of the symbol's bar on `day` (a later call for the
    /// same day replaces it). An id for a day without a bar is kept and used once the bar arrives.
    void add_instrument_id(const std::string& symbol, Day day, const std::string& instrument_id);
    /// The rows PostgresDatabase::get_futures_instrument_ids returns. Returns how many were taken
    /// (a row whose date does not parse is skipped).
    std::size_t add_instrument_ids(const std::vector<market_data_utils::FuturesInstrumentId>& rows);
    /// How many (symbol, day) ids the classifier holds.
    std::size_t instrument_id_count() const;

    SymbolDayVerdict classify_symbol_day(const std::string& symbol, Day date,
                                         const HolidayLookup& holidays) const;

    /// Median volume of the symbol's trailing weekday SESSION bars strictly before `date` (all its
    /// weekday bars while it has no SESSION one); empty when it has no weekday bar before `date`
    /// or when that median is 0. `bars_used` receives how many bars it was taken
    /// over. Each earlier bar's own verdict is computed forward in date order and cached; adding
    /// a bar drops the cached verdicts from its date on. Not safe for concurrent calls (the
    /// runners and the backtest coordinator use one classifier from one thread).
    std::optional<double> norm(const std::string& symbol, Day date, int* bars_used = nullptr) const;

    bool knows(const std::string& symbol) const { return bars_.count(symbol) > 0; }
    const SessionClassifierConfig& config() const { return config_; }

    /// The UTC calendar date of a bar timestamp, the same floor the live price manager's
    /// T-1 test applies (live_price_manager.cpp), so "has a bar dated T-1" means one thing.
    static Day day_of(const Timestamp& ts) { return std::chrono::floor<std::chrono::days>(ts); }
    static std::string ymd(Day day);

private:
    struct DayBar {
        double volume{0.0}, open{0.0}, high{0.0}, low{0.0}, close{0.0};
    };
    /// True when `candidate` is the copy the B0 loader keeps over `held`.
    static bool keeps_over(const DayBar& candidate, const DayBar& held);
    /// The verdict of a bar against a norm (no norm: the floor alone), with the limb in words.
    SessionVerdict judge_bar(const DayBar& bar, const std::optional<double>& norm,
                             std::string* reason) const;
    /// The median over `flags`' SESSION weekday bars of `series` strictly before `date`.
    std::optional<double> session_norm(const std::map<Day, DayBar>& series,
                                       const std::map<Day, bool>& flags, Day date,
                                       int* bars_used) const;
    /// The symbol's per-bar SESSION flags, filled forward in date order up to (not including)
    /// `date`.
    const std::map<Day, bool>& session_flags(const std::string& symbol, Day date) const;

    using Series = std::map<Day, DayBar>;
    /// The id of the symbol's bar at `it`, or nullptr (none known, or `it` is the end).
    const std::string* id_of(const std::string& symbol, const Series& series,
                             Series::const_iterator it) const;
    /// True when the bar at `it` is a confirmed one-day id flip (its previous and next bars exist,
    /// all three ids are known, and its id differs from both).
    bool is_id_flip(const std::string& symbol, const Series& series,
                    Series::const_iterator it) const;
    /// The id of the newest bar before `it` that is not a confirmed flip, or nullptr.
    const std::string* established_id(const std::string& symbol, const Series& series,
                                      Series::const_iterator it) const;
    /// True when the bar at `it` is an unconfirmed id change (reads no bar after `it`); fills
    /// `from` (the previous bar's id) and `established` when given.
    bool is_unconfirmed_id_change(const std::string& symbol, const Series& series,
                                  Series::const_iterator it, std::string* from = nullptr,
                                  std::string* established = nullptr) const;
    /// The day-of limb: an unconfirmed change that id_change_hold_fraction holds.
    bool holds_id_change(const DayBar& bar, const std::optional<double>& norm) const;
    /// Drops every cached SESSION flag of `symbol` from the bar before `day` on (a bar's flag
    /// reads its next bar's id).
    void invalidate_flags_from_previous(const std::string& symbol, Day day);

    SessionClassifierConfig config_;
    std::unordered_map<std::string, std::map<Day, DayBar>> bars_;
    /// T-7b-2 C10a: symbol -> day -> vendor instrument id.
    std::unordered_map<std::string, std::map<Day, std::string>> ids_;
    mutable std::unordered_map<std::string, std::map<Day, bool>> session_flags_;
};

/**
 * @brief Every symbol of a universe classified for one date (the live runners' T-1), and the
 *        book-level roll-up over it.
 *
 * The book-level rule is NOT decided here: it stays "carry the whole book when no symbol has a
 * T-1 price" (the runners' `.empty()` on the T-1 price map, which admits exactly the symbols
 * whose bar is dated T-1, JUNK included). any_printed() is that same predicate on this side.
 */
struct T1Classification {
    std::string t1_date;
    std::vector<SymbolDayVerdict> verdicts;  ///< one per universe symbol, sorted by symbol
    std::size_t session{0}, junk{0}, closure{0}, feed_hole{0};
    int max_hole_age_days{0};
    std::vector<std::string> junk_symbols;       ///< sorted
    std::vector<std::string> feed_hole_symbols;  ///< sorted

    /// Nullptr for a symbol outside the classified universe.
    const SymbolDayVerdict* find(const std::string& symbol) const;
    /// False for a symbol that was not classified: a symbol nobody vouched for is held.
    bool is_session(const std::string& symbol) const;
    bool any_printed() const { return session + junk > 0; }

    /// The block persisted in live_results.config (key "t1_classification"): the date, the four
    /// counts, the longest feed hole, the JUNK and feed-hole symbols, and the symbols whose book
    /// change was held (passed in by the caller, who made the holds).
    nlohmann::json to_json(const std::vector<std::string>& held_symbols = {}) const;
};

T1Classification classify_t1(const SessionClassifier& classifier,
                             const std::vector<std::string>& universe, SessionClassifier::Day t1,
                             const HolidayLookup& holidays);

/**
 * @brief Each distinct symbol of one bar group (the backtest's day), classified at its own bar's
 *        date. Every returned verdict has a bar, so it is SESSION or JUNK. Sorted by symbol.
 *        The backtest uses it on the PREVIOUS group, the bars its signals and fills come from.
 */
std::vector<SymbolDayVerdict> classify_bar_group(const SessionClassifier& classifier,
                                                 const std::vector<Bar>& group,
                                                 const HolidayLookup& holidays = {});

/**
 * @brief LOOP_SPEC v6.1 section 2.1 (K-01): the bars of `bars` a consumer may read, in their order:
 *        every bar whose own verdict (classify_symbol_day at its own date, which reads no later bar)
 *        is not k01_withheld(). The verdicts of the withheld bars are appended to `withheld`, in the
 *        bars' order. The live runners apply it to their whole window on every run, so a withheld
 *        bar is never fed on that run nor on any later one; a bar the classifier does not hold is
 *        kept. The backtest applies the same predicate to each signal group (k01_signal_feed).
 */
std::vector<Bar> k01_consumed_bars(const SessionClassifier& classifier, const std::vector<Bar>& bars,
                                   std::vector<SymbolDayVerdict>* withheld = nullptr);

/**
 * @brief T-7b-2 C10a: the instrument ids the live runners and the backtest feed the classifier,
 *        and the one log line both engines write about it.
 */
struct InstrumentIdFeed {
    bool fed{false};     ///< false: the read failed, the limb judges no bar on this run
    std::size_t ids{0};  ///< ids taken
    std::string line;    ///< "INSTRUMENT_ID_FEED ..."; the caller logs it (INFO fed, WARN not)
};

/// Adds the rows of PostgresDatabase::get_futures_instrument_ids (or reports its error).
InstrumentIdFeed feed_instrument_ids(
    SessionClassifier& classifier,
    const Result<std::vector<market_data_utils::FuturesInstrumentId>>& rows);

}  // namespace trade_ngin
