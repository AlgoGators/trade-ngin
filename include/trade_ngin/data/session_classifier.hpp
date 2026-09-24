// include/trade_ngin/data/session_classifier.hpp
#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "trade_ngin/core/holiday_checker.hpp"
#include "trade_ngin/core/types.hpp"

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

    bool is_session() const { return verdict == SessionVerdict::SESSION; }
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

    SessionClassifierConfig config_;
    std::unordered_map<std::string, std::map<Day, DayBar>> bars_;
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

}  // namespace trade_ngin
